// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// A small backtracking matcher for JSON Schema `pattern` values. It is not
// the A2UI `regex` function dialect (a2ui/regex.h): schema patterns come
// from catalogs and the specification, not from agents, and only need the
// ECMAScript syntax those documents use. See schema.h for the subset.
//
// Stack use: runs of a single-character atom ("[a-z]*", "\\d{6}") are
// matched in a loop, so recursion grows with the pattern's structure and
// with repetitions of groups only. Both are bounded (kMaxGroupDepth), and
// every step counts against kStepBudget.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "a2ui/identifier.h"
#include "a2ui/schema.h"

namespace a2ui {
namespace schema_pattern {
namespace {

constexpr long kStepBudget = 200000;
constexpr int kMaxGroupDepth = 64;
constexpr size_t kMaxPatternLength = 1024;

enum class Prop : uint8_t { Digit, Word, Space, XidStart, XidContinue, Letter, Number };

bool has_prop(Prop p, uint32_t c) {
  switch (p) {
    case Prop::Digit:
      return c >= '0' && c <= '9';
    case Prop::Word:
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    case Prop::Space:
      return c == ' ' || (c >= 0x09 && c <= 0x0D) || c == 0xA0 || c == 0x1680 ||
             (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F ||
             c == 0x205F || c == 0x3000 || c == 0xFEFF;
    case Prop::XidStart:
      return is_xid_start(c);
    case Prop::XidContinue:
      return is_xid_continue(c);
    case Prop::Letter:
      // Approximation: letters are identifier starts other than '_' and
      // letter numbers. Good enough for schema patterns.
      return c != '_' && is_xid_start(c);
    case Prop::Number:
      return (c >= '0' && c <= '9') || (is_xid_continue(c) && !is_xid_start(c) && c > 0x7F);
  }
  return false;
}

struct CharSet {
  bool negated = false;
  std::vector<std::pair<uint32_t, uint32_t>> ranges;
  std::vector<Prop> props;
  std::vector<Prop> neg_props;

  bool contains(uint32_t c) const {
    bool in = false;
    for (const auto& r : ranges) {
      if (c >= r.first && c <= r.second) {
        in = true;
        break;
      }
    }
    if (!in) {
      for (Prop p : props) {
        if (has_prop(p, c)) {
          in = true;
          break;
        }
      }
    }
    if (!in) {
      for (Prop p : neg_props) {
        if (!has_prop(p, c)) {
          in = true;
          break;
        }
      }
    }
    return in != negated;
  }
};

struct Node;
using Seq = std::vector<Node>;

struct Node {
  enum Kind : uint8_t { Set, Any, Start, End, WordBoundary, NotWordBoundary, Group, Repeat };
  Kind kind = Set;
  CharSet set;
  std::vector<Seq> alts;           // Group
  std::unique_ptr<Node> child;     // Repeat
  int min = 0;
  int max = -1;                    // -1 = unbounded
  bool greedy = true;

  Node() = default;
  Node(Node&&) = default;
  Node& operator=(Node&&) = default;

  bool single_char() const { return kind == Set || kind == Any; }
};

// ---------------------------------------------------------------------------
// Parser

class Parser {
 public:
  explicit Parser(const std::vector<uint32_t>& p) : p_(p) {}

  bool parse(std::vector<Seq>* alts) {
    if (!parse_alts(alts, 0)) return false;
    return pos_ == p_.size();
  }

 private:
  bool at_end() const { return pos_ >= p_.size(); }
  uint32_t peek() const { return at_end() ? 0 : p_[pos_]; }

  bool parse_alts(std::vector<Seq>* alts, int depth) {
    if (depth > 32) return false;
    alts->emplace_back();
    while (!at_end()) {
      uint32_t c = peek();
      if (c == '|') {
        ++pos_;
        alts->emplace_back();
        continue;
      }
      if (c == ')') return true;
      Node atom;
      if (!parse_atom(&atom, depth)) return false;
      if (!parse_quantifier(&atom)) return false;
      alts->back().push_back(std::move(atom));
    }
    return true;
  }

  bool parse_int(int* out) {
    if (at_end() || peek() < '0' || peek() > '9') return false;
    long v = 0;
    while (!at_end() && peek() >= '0' && peek() <= '9') {
      v = v * 10 + static_cast<long>(peek() - '0');
      if (v > 100000) return false;
      ++pos_;
    }
    *out = static_cast<int>(v);
    return true;
  }

  bool parse_quantifier(Node* atom) {
    if (at_end()) return true;
    int min = 0, max = -1;
    uint32_t c = peek();
    if (c == '*') {
      ++pos_;
    } else if (c == '+') {
      min = 1;
      ++pos_;
    } else if (c == '?') {
      max = 1;
      ++pos_;
    } else if (c == '{') {
      size_t save = pos_;
      ++pos_;
      if (!parse_int(&min)) {
        pos_ = save;  // A literal '{' (ECMAScript annex B); treat as literal.
        return true;
      }
      max = min;
      if (peek() == ',') {
        ++pos_;
        max = -1;
        if (peek() != '}' && !parse_int(&max)) return false;
      }
      if (peek() != '}') return false;
      ++pos_;
      if (max >= 0 && max < min) return false;
    } else {
      return true;
    }
    if (atom->kind == Node::Start || atom->kind == Node::End || atom->kind == Node::WordBoundary ||
        atom->kind == Node::NotWordBoundary) {
      return false;
    }
    bool greedy = true;
    if (peek() == '?') {
      greedy = false;
      ++pos_;
    }
    Node rep;
    rep.kind = Node::Repeat;
    rep.min = min;
    rep.max = max;
    rep.greedy = greedy;
    rep.child = std::make_unique<Node>(std::move(*atom));
    *atom = std::move(rep);
    return true;
  }

  bool parse_hex(int digits, uint32_t* out) {
    uint32_t v = 0;
    for (int i = 0; i < digits; ++i) {
      if (at_end()) return false;
      uint32_t c = p_[pos_++];
      int d;
      if (c >= '0' && c <= '9') d = static_cast<int>(c - '0');
      else if (c >= 'a' && c <= 'f') d = static_cast<int>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') d = static_cast<int>(c - 'A' + 10);
      else return false;
      v = v * 16 + static_cast<uint32_t>(d);
    }
    *out = v;
    return true;
  }

  bool parse_property(bool negated, CharSet* set) {
    if (peek() != '{') return false;
    ++pos_;
    std::string name;
    while (!at_end() && peek() != '}') {
      if (peek() > 0x7F) return false;
      name.push_back(static_cast<char>(p_[pos_++]));
    }
    if (at_end()) return false;
    ++pos_;
    Prop prop;
    if (name == "XID_Start" || name == "XIDS" || name == "ID_Start" || name == "IDS") {
      prop = Prop::XidStart;
    } else if (name == "XID_Continue" || name == "XIDC" || name == "ID_Continue" || name == "IDC") {
      prop = Prop::XidContinue;
    } else if (name == "L" || name == "Letter" || name == "Alphabetic" || name == "Alpha") {
      prop = Prop::Letter;
    } else if (name == "N" || name == "Nd" || name == "Number") {
      prop = Prop::Number;
    } else {
      return false;
    }
    (negated ? set->neg_props : set->props).push_back(prop);
    return true;
  }

  // Parses an escape after '\'. Single code points go to *cp (returns 1);
  // classes are added to *set (returns 2); word boundaries return 3/4.
  int parse_escape(uint32_t* cp, CharSet* set, bool in_class) {
    if (at_end()) return 0;
    uint32_t c = p_[pos_++];
    switch (c) {
      case 'd': set->props.push_back(Prop::Digit); return 2;
      case 'D': set->neg_props.push_back(Prop::Digit); return 2;
      case 'w': set->props.push_back(Prop::Word); return 2;
      case 'W': set->neg_props.push_back(Prop::Word); return 2;
      case 's': set->props.push_back(Prop::Space); return 2;
      case 'S': set->neg_props.push_back(Prop::Space); return 2;
      case 'p': return parse_property(false, set) ? 2 : 0;
      case 'P': return parse_property(true, set) ? 2 : 0;
      case 'b':
        if (in_class) {
          *cp = 0x08;
          return 1;
        }
        return 3;
      case 'B': return in_class ? 0 : 4;
      case 'n': *cp = '\n'; return 1;
      case 'r': *cp = '\r'; return 1;
      case 't': *cp = '\t'; return 1;
      case 'f': *cp = '\f'; return 1;
      case 'v': *cp = '\v'; return 1;
      case '0': *cp = 0; return 1;
      case 'x': return parse_hex(2, cp) ? 1 : 0;
      case 'u':
        if (peek() == '{') {
          ++pos_;
          uint32_t v = 0;
          int n = 0;
          while (!at_end() && peek() != '}') {
            uint32_t d;
            if (!parse_hex(1, &d)) return 0;
            v = v * 16 + d;
            if (++n > 6) return 0;
          }
          if (at_end() || n == 0 || v > 0x10FFFF) return 0;
          ++pos_;
          *cp = v;
          return 1;
        }
        return parse_hex(4, cp) ? 1 : 0;
      default:
        // Identity escapes of syntax characters (and '-' / '/').
        if ((c < 0x80 && !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9'))) ||
            c > 0x7F) {
          *cp = c;
          return 1;
        }
        return 0;
    }
  }

  bool parse_class(Node* node) {
    node->kind = Node::Set;
    CharSet& set = node->set;
    if (peek() == '^') {
      set.negated = true;
      ++pos_;
    }
    bool first = true;
    while (!at_end()) {
      uint32_t c = peek();
      if (c == ']' && !first) {
        ++pos_;
        return true;
      }
      if (c == ']' && first) {
        // "[]" matches nothing; "[^]" matches anything.
        ++pos_;
        return true;
      }
      first = false;
      uint32_t lo;
      ++pos_;
      if (c == '\\') {
        int r = parse_escape(&lo, &set, true);
        if (r == 0) return false;
        if (r == 2) continue;
      } else {
        lo = c;
      }
      if (peek() == '-' && pos_ + 1 < p_.size() && p_[pos_ + 1] != ']') {
        ++pos_;
        uint32_t hi = p_[pos_++];
        if (hi == '\\') {
          CharSet tmp;
          int r = parse_escape(&hi, &tmp, true);
          if (r != 1) return false;
        }
        if (hi < lo) return false;
        set.ranges.emplace_back(lo, hi);
      } else {
        set.ranges.emplace_back(lo, lo);
      }
    }
    return false;
  }

  bool parse_atom(Node* node, int depth) {
    uint32_t c = p_[pos_++];
    switch (c) {
      case '^': node->kind = Node::Start; return true;
      case '$': node->kind = Node::End; return true;
      case '.':
        node->kind = Node::Any;
        return true;
      case '[':
        return parse_class(node);
      case '(': {
        if (peek() == '?') {
          ++pos_;
          if (peek() == ':') {
            ++pos_;
          } else if (peek() == '<' && pos_ + 1 < p_.size() && p_[pos_ + 1] != '=' &&
                     p_[pos_ + 1] != '!') {
            while (!at_end() && peek() != '>') ++pos_;  // Named group: name ignored.
            if (at_end()) return false;
            ++pos_;
          } else {
            return false;  // Lookaround is not supported.
          }
        }
        node->kind = Node::Group;
        if (!parse_alts(&node->alts, depth + 1)) return false;
        if (peek() != ')') return false;
        ++pos_;
        return true;
      }
      case ')':
      case '*':
      case '+':
      case '?':
      case '|':
        return false;
      case '\\': {
        uint32_t cp = 0;
        node->kind = Node::Set;
        int r = parse_escape(&cp, &node->set, false);
        if (r == 0) return false;
        if (r == 1) node->set.ranges.emplace_back(cp, cp);
        if (r == 3) node->kind = Node::WordBoundary;
        if (r == 4) node->kind = Node::NotWordBoundary;
        return true;
      }
      default:
        node->kind = Node::Set;
        node->set.ranges.emplace_back(c, c);
        return true;
    }
  }

  const std::vector<uint32_t>& p_;
  size_t pos_ = 0;
};

// ---------------------------------------------------------------------------
// Matcher

struct Cont {
  enum Kind : uint8_t { SeqK, RepK };
  Kind kind;
  // SeqK: continue at seq[idx].
  const Seq* seq;
  size_t idx;
  // RepK: one more iteration of `rep` finished; `count` done so far, started at `start`.
  const Node* rep;
  int count;
  size_t start;
  const Cont* next;
};

class Matcher {
 public:
  explicit Matcher(const std::vector<uint32_t>& text) : t_(text) {}

  bool budget_exhausted() const { return steps_ < 0; }

  bool match_seq(const Seq& seq, size_t i, size_t pos, const Cont* cont, int depth) {
    for (;;) {
      if (--steps_ < 0) return false;
      if (i == seq.size()) return resume(pos, cont, depth);
      const Node& n = seq[i];
      switch (n.kind) {
        case Node::Set:
        case Node::Any:
          if (pos >= t_.size() || !char_matches(n, t_[pos])) return false;
          ++pos;
          ++i;
          continue;
        case Node::Start:
          if (pos != 0) return false;
          ++i;
          continue;
        case Node::End:
          if (pos != t_.size()) return false;
          ++i;
          continue;
        case Node::WordBoundary:
        case Node::NotWordBoundary: {
          bool a = pos > 0 && has_prop(Prop::Word, t_[pos - 1]);
          bool b = pos < t_.size() && has_prop(Prop::Word, t_[pos]);
          if ((a != b) != (n.kind == Node::WordBoundary)) return false;
          ++i;
          continue;
        }
        case Node::Group: {
          if (depth > kMaxGroupDepth) return fail_budget();
          Cont c{Cont::SeqK, &seq, i + 1, nullptr, 0, 0, cont};
          for (const auto& alt : n.alts) {
            if (match_seq(alt, 0, pos, &c, depth + 1)) return true;
            if (steps_ < 0) return false;
          }
          return false;
        }
        case Node::Repeat: {
          if (n.child->single_char()) {
            size_t run = 0;
            const size_t limit = n.max < 0 ? t_.size() - pos : static_cast<size_t>(n.max);
            while (run < limit && pos + run < t_.size() && char_matches(*n.child, t_[pos + run])) {
              ++run;
            }
            if (run < static_cast<size_t>(n.min)) return false;
            if (n.greedy) {
              for (size_t k = run + 1; k-- > static_cast<size_t>(n.min);) {
                if (match_seq(seq, i + 1, pos + k, cont, depth + 1)) return true;
                if (steps_ < 0) return false;
              }
            } else {
              for (size_t k = static_cast<size_t>(n.min); k <= run; ++k) {
                if (match_seq(seq, i + 1, pos + k, cont, depth + 1)) return true;
                if (steps_ < 0) return false;
              }
            }
            return false;
          }
          Cont after{Cont::SeqK, &seq, i + 1, nullptr, 0, 0, cont};
          return repeat(n, 0, pos, &after, depth + 1);
        }
      }
    }
  }

 private:
  bool fail_budget() {
    steps_ = -1;
    return false;
  }

  bool char_matches(const Node& n, uint32_t c) const {
    if (n.kind == Node::Any) return c != '\n' && c != '\r' && c != 0x2028 && c != 0x2029;
    return n.set.contains(c);
  }

  // Tries iteration `count + 1` of `rep` at `pos`, or leaving the loop.
  bool repeat(const Node& rep, int count, size_t pos, const Cont* after, int depth) {
    if (depth > kMaxGroupDepth) return fail_budget();
    const bool can_stop = count >= rep.min;
    const bool can_more = rep.max < 0 || count < rep.max;
    Cont again{Cont::RepK, nullptr, 0, &rep, count + 1, pos, after};
    if (rep.greedy) {
      if (can_more && match_child(rep, pos, &again, depth)) return true;
      if (steps_ < 0) return false;
      return can_stop && resume(pos, after, depth);
    }
    if (can_stop && resume(pos, after, depth)) return true;
    if (steps_ < 0) return false;
    return can_more && match_child(rep, pos, &again, depth);
  }

  bool match_child(const Node& rep, size_t pos, const Cont* cont, int depth) {
    const Node& child = *rep.child;
    if (child.kind == Node::Group) {
      for (const auto& alt : child.alts) {
        if (match_seq(alt, 0, pos, cont, depth + 1)) return true;
        if (steps_ < 0) return false;
      }
      return false;
    }
    // Nested repeat ("(?:a*)*" style is parsed as group; a bare repeat child
    // only arises from "a**", which the parser rejects). Anchors cannot repeat.
    return false;
  }

  bool resume(size_t pos, const Cont* cont, int depth) {
    if (cont == nullptr) return true;
    if (cont->kind == Cont::SeqK) return match_seq(*cont->seq, cont->idx, pos, cont->next, depth + 1);
    // RepK: an iteration ended. An empty iteration cannot loop again.
    if (pos == cont->start) {
      return cont->count >= cont->rep->min ? resume(pos, cont->next, depth + 1)
                                           : repeat(*cont->rep, cont->rep->min, pos, cont->next,
                                                    depth + 1);
    }
    return repeat(*cont->rep, cont->count, pos, cont->next, depth + 1);
  }

  const std::vector<uint32_t>& t_;
  long steps_ = kStepBudget;
};

std::vector<uint32_t> decode(std::string_view s) {
  std::vector<uint32_t> out;
  out.reserve(s.size());
  size_t pos = 0;
  while (pos < s.size()) out.push_back(decode_utf8(s, pos));
  return out;
}

}  // namespace

bool search(std::string_view pattern, std::string_view text, bool* ok) {
  if (ok) *ok = true;
  if (pattern.size() > kMaxPatternLength) {
    if (ok) *ok = false;
    return false;
  }
  const std::vector<uint32_t> p = decode(pattern);
  std::vector<Seq> alts;
  Parser parser(p);
  if (!parser.parse(&alts)) {
    if (ok) *ok = false;
    return false;
  }
  Node root;
  root.kind = Node::Group;
  root.alts = std::move(alts);
  Seq top;
  top.push_back(std::move(root));

  const std::vector<uint32_t> t = decode(text);
  Matcher m(t);
  for (size_t start = 0; start <= t.size(); ++start) {
    if (m.match_seq(top, 0, start, nullptr, 0)) return true;
    if (m.budget_exhausted()) {
      if (ok) *ok = false;
      return false;
    }
  }
  return false;
}

}  // namespace schema_pattern
}  // namespace a2ui

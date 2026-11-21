// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Parser, static safety guard and code generator for the A2UI regex dialect.
// Ported from the reference SDKs (Python regex_subset.py, Swift
// RegexSubset.swift); accept/reject decisions and reason tokens match them.
//
// Nothing here recurses on pattern structure, so stack use does not depend
// on the pattern: the parser keeps open groups on a heap stack, and AST nodes
// are created in post-order (children before parents), which lets every
// analysis and the code generator run as one forward pass over the nodes.

#include <algorithm>
#include <cstdio>

#include "a2ui/identifier.h"
#include "regex_internal.h"

namespace a2ui {
namespace regex {
namespace {

constexpr uint32_t kInf = 0xFFFFFFFFu;  // Unbounded repeat maximum.
constexpr uint32_t kMaxQuantifierBound = 65535;
constexpr uint32_t kNoCp = 0xFFFFFFFFu;  // peek() past the end of the pattern.
constexpr uint32_t kSurrogateStart = 0xD800;
constexpr uint32_t kSurrogateEnd = 0xDFFF;
constexpr uint32_t kMaxCodePoint = 0x10FFFF;

using RangeSet = std::vector<Range>;

// ---------------------------------------------------------------------------
// Code-point range sets (closed intervals, normalized = sorted, merged and
// without surrogates).

void append_without_surrogates(RangeSet& out, Range r) {
  if (r.lo > r.hi) return;
  if (r.hi < kSurrogateStart || r.lo > kSurrogateEnd) {
    out.push_back(r);
    return;
  }
  if (r.lo < kSurrogateStart) out.push_back({r.lo, kSurrogateStart - 1});
  if (r.hi > kSurrogateEnd) out.push_back({kSurrogateEnd + 1, r.hi});
}

RangeSet normalize(const RangeSet& in) {
  RangeSet split;
  split.reserve(in.size() + 1);
  for (const Range& r : in) append_without_surrogates(split, r);
  if (split.size() <= 1) return split;
  std::sort(split.begin(), split.end(), [](const Range& x, const Range& y) {
    return x.lo != y.lo ? x.lo < y.lo : x.hi < y.hi;
  });
  RangeSet merged;
  merged.push_back(split[0]);
  for (size_t i = 1; i < split.size(); ++i) {
    Range& prev = merged.back();
    if (split[i].lo <= prev.hi + 1) {
      prev.hi = std::max(prev.hi, split[i].hi);
    } else {
      merged.push_back(split[i]);
    }
  }
  return merged;
}

/** The complement over all Unicode scalar values (never includes surrogates). */
RangeSet complement(const RangeSet& ranges) {
  RangeSet out;
  uint32_t cursor = 0;
  for (const Range& r : normalize(ranges)) {
    if (r.lo > cursor) append_without_surrogates(out, {cursor, r.lo - 1});
    cursor = r.hi + 1;
  }
  if (cursor <= kMaxCodePoint) append_without_surrogates(out, {cursor, kMaxCodePoint});
  return out;
}

bool overlap(const RangeSet& a, const RangeSet& b) {
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (a[i].hi < b[j].lo) {
      ++i;
    } else if (b[j].hi < a[i].lo) {
      ++j;
    } else {
      return true;
    }
  }
  return false;
}

void union_into(RangeSet& acc, const RangeSet& more) {
  acc.insert(acc.end(), more.begin(), more.end());
}

const RangeSet& digit_ranges() {
  static const RangeSet r = {{0x30, 0x39}};
  return r;
}
const RangeSet& word_ranges() {
  static const RangeSet r = {{0x30, 0x39}, {0x41, 0x5A}, {0x5F, 0x5F}, {0x61, 0x7A}};
  return r;
}
const RangeSet& space_ranges() {
  static const RangeSet r = {{0x09, 0x0D},     {0x20, 0x20},     {0xA0, 0xA0},
                             {0x1680, 0x1680}, {0x2000, 0x200A}, {0x2028, 0x2029},
                             {0x202F, 0x202F}, {0x205F, 0x205F}, {0x3000, 0x3000},
                             {0xFEFF, 0xFEFF}};
  return r;
}
RangeSet dot_ranges() { return complement({{0x0A, 0x0A}, {0x0D, 0x0D}, {0x2028, 0x2029}}); }

/** \d \D \w \W \s \S, or false. */
bool class_escape(uint32_t cp, RangeSet& out) {
  switch (cp) {
    case 'd': out = digit_ranges(); return true;
    case 'D': out = complement(digit_ranges()); return true;
    case 'w': out = word_ranges(); return true;
    case 'W': out = complement(word_ranges()); return true;
    case 's': out = space_ranges(); return true;
    case 'S': out = complement(space_ranges()); return true;
    default: return false;
  }
}

bool is_syntax_char(uint32_t cp) {
  switch (cp) {
    case '^': case '$': case '\\': case '.': case '*': case '+': case '?':
    case '(': case ')': case '[': case ']': case '{': case '}': case '|': case '/':
      return true;
    default:
      return false;
  }
}

bool is_quantifier_start(uint32_t cp) { return cp == '*' || cp == '+' || cp == '?' || cp == '{'; }

bool is_ascii_digit(uint32_t cp) { return cp >= '0' && cp <= '9'; }

int hex_value(uint32_t cp) {
  if (cp >= '0' && cp <= '9') return static_cast<int>(cp - '0');
  if (cp >= 'A' && cp <= 'F') return static_cast<int>(cp - 'A' + 10);
  if (cp >= 'a' && cp <= 'f') return static_cast<int>(cp - 'a' + 10);
  return -1;
}

// ---------------------------------------------------------------------------
// AST

enum class Kind : uint8_t { Empty, CharSet, Group, Look, Anchor, Concat, Alt, Repeat };

struct Node {
  Kind kind = Kind::Empty;
  uint8_t flag = 0;  // Anchor kind, or 1 for a negative lookahead.
  uint32_t min = 0, max = 0;  // Repeat bounds; max == kInf when unbounded.
  std::vector<int> kids;      // One for Group/Look/Repeat; many for Concat/Alt.
  RangeSet ranges;            // CharSet.
};

// ---------------------------------------------------------------------------
// Parser

class Parser {
 public:
  explicit Parser(std::vector<uint32_t> cps) : cps_(std::move(cps)) {}

  /** Parses the whole pattern; returns the root node or -1 (see error()). */
  int parse() {
    frames_.push_back(Frame{});
    while (!eof()) {
      const uint32_t cp = peek();
      if (cp == '|') {
        next();
        end_alternative(frames_.back());
        continue;
      }
      if (cp == ')') {
        if (frames_.size() == 1) return fail("Unmatched closing parenthesis");
        next();
        Frame frame = std::move(frames_.back());
        frames_.pop_back();
        const int body = end_disjunction(frame);
        Node n;
        n.kind = frame.lookahead ? Kind::Look : Kind::Group;
        n.flag = frame.negated ? 1 : 0;
        n.kids.push_back(body);
        if (!add_term(add(std::move(n)))) return -1;
        continue;
      }
      if (cp == '(') {
        next();
        Frame frame;
        if (eat('?')) {
          const uint32_t modifier = peek();
          if (modifier != ':' && modifier != '=' && modifier != '!') {
            return fail("Unsupported group extension");
          }
          next();
          frame.lookahead = modifier != ':';
          frame.negated = modifier == '!';
        }
        frames_.push_back(std::move(frame));
        continue;
      }
      const int atom = parse_atom();
      if (atom < 0 || !add_term(atom)) return -1;
    }
    if (frames_.size() > 1) return fail("Unclosed group");
    return end_disjunction(frames_.back());
  }

  std::vector<Node>& nodes() { return nodes_; }
  const std::string& error() const { return error_; }

 private:
  struct Frame {
    bool lookahead = false;
    bool negated = false;
    std::vector<int> branches;
    std::vector<int> items;
  };

  enum class EscKind { Single, Ranges, Anchor };
  struct Escape {
    EscKind kind = EscKind::Single;
    uint32_t cp = 0;
    RangeSet ranges;
    uint8_t anchor = 0;
  };

  bool eof() const { return pos_ >= cps_.size(); }
  uint32_t peek(size_t offset = 0) const {
    return pos_ + offset < cps_.size() ? cps_[pos_ + offset] : kNoCp;
  }
  uint32_t next() { return cps_[pos_++]; }  // Callers check eof() first.
  bool eat(uint32_t cp) {
    if (peek() != cp) return false;
    ++pos_;
    return true;
  }

  int fail(std::string message) {
    if (error_.empty()) error_ = std::move(message);
    return -1;
  }

  int add(Node n) {
    nodes_.push_back(std::move(n));
    return static_cast<int>(nodes_.size()) - 1;
  }

  int charset(RangeSet ranges) {
    Node n;
    n.kind = Kind::CharSet;
    n.ranges = std::move(ranges);
    return add(std::move(n));
  }

  int anchor(uint8_t kind) {
    Node n;
    n.kind = Kind::Anchor;
    n.flag = kind;
    return add(std::move(n));
  }

  void end_alternative(Frame& f) {
    if (f.items.empty()) {
      f.branches.push_back(add(Node{}));
    } else if (f.items.size() == 1) {
      f.branches.push_back(f.items[0]);
    } else {
      Node n;
      n.kind = Kind::Concat;
      n.kids = std::move(f.items);
      f.branches.push_back(add(std::move(n)));
    }
    f.items.clear();
  }

  int end_disjunction(Frame& f) {
    end_alternative(f);
    if (f.branches.size() == 1) return f.branches[0];
    Node n;
    n.kind = Kind::Alt;
    n.kids = std::move(f.branches);
    return add(std::move(n));
  }

  /** Applies a following quantifier, if any, then appends the term. */
  bool add_term(int atom) {
    if (is_quantifier_start(peek())) {
      const Kind k = nodes_[atom].kind;
      if (k == Kind::Anchor || k == Kind::Look) {
        return fail("Quantifier on an assertion is not supported") >= 0;
      }
      atom = parse_quantifier(atom);
      if (atom < 0) return false;
    }
    frames_.back().items.push_back(atom);
    return true;
  }

  int parse_quantifier(int child) {
    const uint32_t q = next();
    uint32_t min = 0, max = kInf;
    if (q == '+') {
      min = 1;
    } else if (q == '?') {
      max = 1;
    } else if (q == '{') {
      if (!parse_braced_bounds(min, max)) return -1;
    }
    eat('?');  // Lazy: irrelevant to a boolean search.
    if (is_quantifier_start(peek())) {
      return fail("Possessive or chained quantifiers are not supported");
    }
    Node n;
    n.kind = Kind::Repeat;
    n.min = min;
    n.max = max;
    n.kids.push_back(child);
    return add(std::move(n));
  }

  /** Reads ASCII digits (saturating); false if there are none. */
  bool parse_decimal(uint32_t& value) {
    if (!is_ascii_digit(peek())) return false;
    uint64_t v = 0;
    while (is_ascii_digit(peek())) {
      v = std::min<uint64_t>(v * 10 + (next() - '0'), 0xFFFFFFFFull);
    }
    value = static_cast<uint32_t>(v);
    return true;
  }

  static std::string bound_message(const char* which, uint32_t v) {
    char buf[80];
    std::snprintf(buf, sizeof(buf), "Quantifier %s bound %lu exceeds %lu", which,
                  static_cast<unsigned long>(v), static_cast<unsigned long>(kMaxQuantifierBound));
    return buf;
  }

  bool parse_braced_bounds(uint32_t& min, uint32_t& max) {
    if (!parse_decimal(min)) return fail("Malformed brace quantifier") >= 0;
    if (min > kMaxQuantifierBound) return fail(bound_message("lower", min)) >= 0;
    if (eat('}')) {
      max = min;
      return true;
    }
    if (!eat(',')) return fail("Malformed brace quantifier") >= 0;
    if (eat('}')) {
      max = kInf;
      return true;
    }
    if (!parse_decimal(max) || !eat('}')) return fail("Malformed brace quantifier") >= 0;
    if (max > kMaxQuantifierBound) return fail(bound_message("upper", max)) >= 0;
    if (max < min) return fail("Quantifier minimum exceeds maximum") >= 0;
    return true;
  }

  int parse_atom() {
    const uint32_t cp = next();
    switch (cp) {
      case '^': return anchor(kAnchorStart);
      case '$': return anchor(kAnchorEnd);
      case '.': return charset(dot_ranges());
      case '[': return parse_class();
      case '\\': {
        Escape esc;
        if (!parse_escape(false, esc)) return -1;
        if (esc.kind == EscKind::Anchor) return anchor(esc.anchor);
        if (esc.kind == EscKind::Ranges) return charset(std::move(esc.ranges));
        return charset({{esc.cp, esc.cp}});
      }
      case '{': case '}': case ']':
        return fail("Unescaped brace or bracket is not supported");
      case '*': case '+': case '?':
        return fail("Nothing to repeat");
      default:
        return charset({{cp, cp}});
    }
  }

  bool parse_fixed_hex(int digits, uint32_t& value) {
    value = 0;
    for (int i = 0; i < digits; ++i) {
      const int hv = hex_value(peek());
      if (hv < 0) return fail("Invalid hexadecimal escape") >= 0;
      next();
      value = (value << 4) | static_cast<uint32_t>(hv);
    }
    return true;
  }

  bool reject_surrogate(uint32_t cp) {
    if (cp >= kSurrogateStart && cp <= kSurrogateEnd) {
      return fail("Surrogate code point escape is not supported") >= 0;
    }
    return true;
  }

  bool parse_unicode_escape(uint32_t& value) {
    if (!eat('{')) return parse_fixed_hex(4, value) && reject_surrogate(value);
    value = 0;
    int count = 0;
    while (!eof() && peek() != '}') {
      const int hv = hex_value(peek());
      if (hv < 0) return fail("Invalid braced Unicode escape") >= 0;
      next();
      value = value * 16 + static_cast<uint32_t>(hv);
      ++count;
      if (value > kMaxCodePoint) return fail("Unicode escape exceeds U+10FFFF") >= 0;
    }
    if (count == 0 || !eat('}')) return fail("Malformed braced Unicode escape") >= 0;
    return reject_surrogate(value);
  }

  bool parse_escape(bool in_class, Escape& out) {
    if (eof()) return fail("Trailing backslash in pattern") >= 0;
    const uint32_t cp = next();
    out.kind = EscKind::Single;
    if (cp == 'b') {
      if (in_class) {
        out.cp = 0x08;
      } else {
        out.kind = EscKind::Anchor;
        out.anchor = kWordBoundary;
      }
      return true;
    }
    if (cp == 'B') {
      if (in_class) return fail("\\B is not supported inside a character class") >= 0;
      out.kind = EscKind::Anchor;
      out.anchor = kNotWordBoundary;
      return true;
    }
    if (class_escape(cp, out.ranges)) {
      out.kind = EscKind::Ranges;
      return true;
    }
    switch (cp) {
      case 't': out.cp = 0x09; return true;
      case 'n': out.cp = 0x0A; return true;
      case 'v': out.cp = 0x0B; return true;
      case 'f': out.cp = 0x0C; return true;
      case 'r': out.cp = 0x0D; return true;
      case '0':
        if (is_ascii_digit(peek())) return fail("Octal escape is not supported") >= 0;
        out.cp = 0;
        return true;
      case 'x': return parse_fixed_hex(2, out.cp);
      case 'u': return parse_unicode_escape(out.cp);
      default: break;
    }
    if (cp >= '1' && cp <= '9') {
      return fail("Backreference or octal escape is not supported") >= 0;
    }
    if (is_syntax_char(cp) || (in_class && cp == '-')) {
      out.cp = cp;
      return true;
    }
    return fail("Unsupported escape sequence") >= 0;
  }

  /** One class atom: `single` is set for a single code point (kNoCp for an escape class). */
  bool parse_class_atom(uint32_t& single, RangeSet& ranges) {
    const uint32_t cp = peek();
    if (cp == '\\') {
      next();
      Escape esc;
      if (!parse_escape(true, esc)) return false;
      if (esc.kind == EscKind::Single) {
        single = esc.cp;
        ranges = {{esc.cp, esc.cp}};
      } else {
        single = kNoCp;
        ranges = std::move(esc.ranges);
      }
      return true;
    }
    if (cp == '[') return fail("Nested character class is not supported") >= 0;
    if (cp == '&' && peek(1) == '&') {
      return fail("Character class intersection is not supported") >= 0;
    }
    if (eof()) return fail("Unexpected end of pattern") >= 0;
    single = next();
    ranges = {{single, single}};
    return true;
  }

  bool parse_class_item(RangeSet& acc) {
    uint32_t first = kNoCp;
    RangeSet first_ranges;
    if (!parse_class_atom(first, first_ranges)) return false;
    if (peek() != '-' || peek(1) == ']') {
      union_into(acc, first_ranges);
      return true;
    }
    next();
    if (peek() == '-') return fail("Character class subtraction is not supported") >= 0;
    uint32_t second = kNoCp;
    RangeSet second_ranges;
    if (!parse_class_atom(second, second_ranges)) return false;
    if (first == kNoCp || second == kNoCp) {
      return fail("Character class escape cannot form a range") >= 0;
    }
    if (first > second) return fail("Character class range out of order") >= 0;
    acc.push_back({first, second});
    return true;
  }

  int parse_class() {
    const bool negated = eat('^');
    RangeSet ranges;
    while (!eof() && peek() != ']') {
      if (!parse_class_item(ranges)) return -1;
    }
    if (!eat(']')) return fail("Unclosed character class");
    if (ranges.empty()) return fail("Empty character class");
    RangeSet normalized = normalize(ranges);
    return charset(negated ? complement(normalized) : std::move(normalized));
  }

  std::vector<uint32_t> cps_;
  size_t pos_ = 0;
  std::vector<Node> nodes_;
  std::vector<Frame> frames_;
  std::string error_;
};

// ---------------------------------------------------------------------------
// Static safety guard. Each property is computed for every node in one
// forward pass, since children always precede their parents.

struct Facts {
  std::vector<bool> nullable;   // Can match "".
  std::vector<bool> unbounded;  // Contains a quantifier with no maximum.
  std::vector<RangeSet> first;  // Code points that can start a non-empty match.
  std::vector<RangeSet> chars;  // Every code point consumed outside lookaheads.
};

bool is_unbounded_repeat(const Node& n) { return n.kind == Kind::Repeat && n.max == kInf; }

int unwrap(const std::vector<Node>& nodes, int i) {
  while (nodes[i].kind == Kind::Group) i = nodes[i].kids[0];
  return i;
}

Facts analyze(const std::vector<Node>& nodes) {
  const size_t n = nodes.size();
  Facts f;
  f.nullable.resize(n);
  f.unbounded.resize(n);
  f.first.resize(n);
  f.chars.resize(n);
  for (size_t i = 0; i < n; ++i) {
    const Node& node = nodes[i];
    bool unbounded = is_unbounded_repeat(node);
    for (int k : node.kids) unbounded = unbounded || f.unbounded[k];
    f.unbounded[i] = unbounded;
    switch (node.kind) {
      case Kind::Empty:
      case Kind::Anchor:
      case Kind::Look:
        f.nullable[i] = true;
        break;
      case Kind::CharSet:
        f.nullable[i] = false;
        f.first[i] = node.ranges;
        f.chars[i] = node.ranges;
        break;
      case Kind::Group:
        f.nullable[i] = f.nullable[node.kids[0]];
        f.first[i] = f.first[node.kids[0]];
        f.chars[i] = f.chars[node.kids[0]];
        break;
      case Kind::Repeat:
        f.nullable[i] = node.min == 0 || f.nullable[node.kids[0]];
        if (node.max != 0) {
          f.first[i] = f.first[node.kids[0]];
          f.chars[i] = f.chars[node.kids[0]];
        }
        break;
      case Kind::Alt: {
        bool any = false;
        RangeSet first, chars;
        for (int k : node.kids) {
          any = any || f.nullable[k];
          union_into(first, f.first[k]);
          union_into(chars, f.chars[k]);
        }
        f.nullable[i] = any;
        f.first[i] = normalize(first);
        f.chars[i] = normalize(chars);
        break;
      }
      case Kind::Concat: {
        bool all = true;
        RangeSet first, chars;
        for (int k : node.kids) {
          if (all) union_into(first, f.first[k]);
          all = all && f.nullable[k];
          union_into(chars, f.chars[k]);
        }
        f.nullable[i] = all;
        f.first[i] = normalize(first);
        f.chars[i] = normalize(chars);
        break;
      }
    }
  }
  return f;
}

/** Rule 2: a quantifier allowing more than one repetition over an unbounded one. */
bool has_nested_quantifier(const std::vector<Node>& nodes, const Facts& f) {
  for (const Node& n : nodes) {
    if (n.kind == Kind::Repeat && (n.max == kInf || n.max > 1) && f.unbounded[n.kids[0]]) {
      return true;
    }
  }
  return false;
}

/** Rule 3: an unbounded repeat over an alternation whose branches can start alike. */
bool has_overlapping_alternation(const std::vector<Node>& nodes, const Facts& f) {
  for (const Node& n : nodes) {
    if (!is_unbounded_repeat(n)) continue;
    const Node& inner = nodes[unwrap(nodes, n.kids[0])];
    if (inner.kind != Kind::Alt) continue;
    for (size_t a = 0; a < inner.kids.size(); ++a) {
      for (size_t b = a + 1; b < inner.kids.size(); ++b) {
        if (overlap(f.first[inner.kids[a]], f.first[inner.kids[b]])) return true;
      }
    }
  }
  return false;
}

/** Rule 4: two unbounded repeats over overlapping sets, separated only by nullable items. */
bool has_adjacent_overlap(const std::vector<Node>& nodes, const Facts& f) {
  for (const Node& n : nodes) {
    if (n.kind != Kind::Concat) continue;
    for (size_t i = 0; i < n.kids.size(); ++i) {
      const Node& x = nodes[unwrap(nodes, n.kids[i])];
      if (!is_unbounded_repeat(x)) continue;
      const RangeSet& x_chars = f.chars[x.kids[0]];
      for (size_t j = i + 1; j < n.kids.size(); ++j) {
        const Node& y = nodes[unwrap(nodes, n.kids[j])];
        if (is_unbounded_repeat(y) && overlap(x_chars, f.chars[y.kids[0]])) return true;
        if (!f.nullable[n.kids[j]]) break;
      }
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Code generation. Jumps are relative, so a node's code block can be copied
// (for counted repeats) or moved into its parent without relocation.

using Code = std::vector<Inst>;

Inst inst(Op op, int a = 0, int b = 0, uint8_t flag = 0) {
  return Inst{op, flag, static_cast<int16_t>(a), static_cast<uint16_t>(b)};
}

/** Size in instructions of each node's code, saturating above kMaxProgramSize. */
bool program_fits(const std::vector<Node>& nodes, int root) {
  constexpr uint64_t kCap = kMaxProgramSize + 1;
  std::vector<uint64_t> size(nodes.size());
  for (size_t i = 0; i < nodes.size(); ++i) {
    const Node& n = nodes[i];
    uint64_t s = 0;
    switch (n.kind) {
      case Kind::Empty: s = 0; break;
      case Kind::CharSet:
      case Kind::Anchor: s = 1; break;
      case Kind::Group: s = size[n.kids[0]]; break;
      case Kind::Look: s = size[n.kids[0]] + 2; break;
      case Kind::Concat:
        for (int k : n.kids) s += size[k];
        break;
      case Kind::Alt:
        for (int k : n.kids) s += size[k];
        s += 2 * (n.kids.size() - 1);
        break;
      case Kind::Repeat: {
        const uint64_t c = size[n.kids[0]];
        s = n.min * c + (n.max == kInf ? c + 2 : (uint64_t{n.max} - n.min) * (c + 1));
        break;
      }
    }
    size[i] = std::min(s, kCap);
  }
  return size[root] + 1 <= kMaxProgramSize;  // + the final Match.
}

void generate(std::vector<Node>& nodes, int root, Program& out) {
  std::vector<Code> code(nodes.size());
  std::vector<int> look_depth(nodes.size());
  for (size_t i = 0; i < nodes.size(); ++i) {
    Node& n = nodes[i];
    Code c;
    int depth = 0;
    for (int k : n.kids) depth = std::max(depth, look_depth[k]);
    switch (n.kind) {
      case Kind::Empty:
        break;
      case Kind::CharSet:
        c.push_back(inst(Op::Char, static_cast<int>(out.ranges.size()),
                         static_cast<int>(n.ranges.size())));
        out.ranges.insert(out.ranges.end(), n.ranges.begin(), n.ranges.end());
        break;
      case Kind::Anchor:
        c.push_back(inst(Op::Assert, 0, 0, n.flag));
        break;
      case Kind::Group:
        c = std::move(code[n.kids[0]]);
        break;
      case Kind::Look: {
        Code& body = code[n.kids[0]];
        c.push_back(inst(Op::Look, static_cast<int>(body.size()) + 2, 0, n.flag));
        c.insert(c.end(), body.begin(), body.end());
        c.push_back(inst(Op::Match));
        ++depth;
        break;
      }
      case Kind::Concat:
        for (int k : n.kids) c.insert(c.end(), code[k].begin(), code[k].end());
        break;
      case Kind::Alt: {
        size_t total = 0;
        for (int k : n.kids) total += code[k].size() + 2;
        total -= 2;
        for (size_t b = 0; b < n.kids.size(); ++b) {
          const Code& branch = code[n.kids[b]];
          const bool last = b + 1 == n.kids.size();
          if (!last) c.push_back(inst(Op::Split, 1, static_cast<int>(branch.size()) + 2));
          c.insert(c.end(), branch.begin(), branch.end());
          if (!last) c.push_back(inst(Op::Jmp, static_cast<int>(total - c.size())));
        }
        break;
      }
      case Kind::Repeat: {
        const Code& body = code[n.kids[0]];
        const int s = static_cast<int>(body.size());
        for (uint32_t r = 0; r < n.min; ++r) c.insert(c.end(), body.begin(), body.end());
        if (n.max == kInf) {
          c.push_back(inst(Op::Split, 1, s + 2));
          c.insert(c.end(), body.begin(), body.end());
          c.push_back(inst(Op::Jmp, -(s + 1)));
        } else {
          for (uint32_t r = n.min; r < n.max; ++r) {
            c.push_back(inst(Op::Split, 1, s + 1));
            c.insert(c.end(), body.begin(), body.end());
          }
        }
        break;
      }
    }
    for (int k : n.kids) Code().swap(code[k]);  // Free children as we go.
    look_depth[i] = depth;
    code[i] = std::move(c);
  }
  out.code = std::move(code[root]);
  out.code.push_back(inst(Op::Match));
  out.look_depth = look_depth[root];
}

}  // namespace

const char* rejection_token(Rejection r) {
  switch (r) {
    case Rejection::None: return "OK";
    case Rejection::UnsupportedSyntax: return "UNSUPPORTED_SYNTAX";
    case Rejection::PatternTooLong: return "PATTERN_TOO_LONG";
    case Rejection::NestedQuantifier: return "NESTED_QUANTIFIER";
    case Rejection::OverlappingAlternation: return "OVERLAPPING_ALTERNATION";
    case Rejection::AdjacentOverlap: return "ADJACENT_OVERLAP";
  }
  return "UNSUPPORTED_SYNTAX";
}

CompileResult compile_program(std::string_view pattern, Program& out) {
  CompileResult result;
  auto reject = [&result](Rejection r, std::string detail) {
    result.rejection = r;
    result.detail = std::move(detail);
    return result;
  };
  if (utf8_length(pattern) > kMaxPatternCodePoints) {
    return reject(Rejection::PatternTooLong, "Pattern exceeds maximum length of " +
                                                 std::to_string(kMaxPatternCodePoints) +
                                                 " code points");
  }
  if (!utf8_valid(pattern)) return reject(Rejection::UnsupportedSyntax, "Invalid UTF-8 in pattern");

  std::vector<uint32_t> cps;
  cps.reserve(pattern.size());
  for (size_t pos = 0; pos < pattern.size();) cps.push_back(decode_utf8(pattern, pos));

  Parser parser(std::move(cps));
  const int root = parser.parse();
  if (root < 0) return reject(Rejection::UnsupportedSyntax, parser.error());
  std::vector<Node>& nodes = parser.nodes();

  {
    const Facts facts = analyze(nodes);
    Rejection r = Rejection::None;
    if (has_nested_quantifier(nodes, facts)) {
      r = Rejection::NestedQuantifier;
    } else if (has_overlapping_alternation(nodes, facts)) {
      r = Rejection::OverlappingAlternation;
    } else if (has_adjacent_overlap(nodes, facts)) {
      r = Rejection::AdjacentOverlap;
    }
    if (r != Rejection::None) {
      return reject(r, std::string("Unsafe regex pattern (") + rejection_token(r) + ")");
    }
  }

  if (!program_fits(nodes, root)) {
    return reject(Rejection::PatternTooLong, "Pattern expands to more than " +
                                                 std::to_string(kMaxProgramSize) +
                                                 " instructions");
  }
  generate(nodes, root, out);
  if (out.look_depth > kMaxLookaheadNesting) {
    out = Program();
    return reject(Rejection::UnsupportedSyntax, "Lookaheads nest more than " +
                                                    std::to_string(kMaxLookaheadNesting) +
                                                    " deep");
  }
  return result;
}

}  // namespace regex
}  // namespace a2ui

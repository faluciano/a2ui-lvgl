// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Pike VM for compiled A2UI regex programs. All live NFA states advance in
// lockstep over the input, so a search takes O(input * program) steps with no
// backtracking. A lookahead is checked by an anchored sub-simulation from the
// current position; each lookahead instruction is checked at most once per
// position, so a lookahead reachable at every position (an unanchored
// `(?=.*x)`) costs O(input^2 * program) in the worst case. Anchored uses such
// as `^(?=.*\d).{8,}$` stay linear. Recursion depth is the lookahead nesting depth of the pattern
// (at most kMaxLookaheadNesting), never a function of the input.
//
// Memory per search: two state sets and a work stack per lookahead level,
// each sized to the program (uint16 entries), all on the heap.

#include <utility>

#include "a2ui/identifier.h"
#include "regex_internal.h"

namespace a2ui {
namespace regex {
namespace {

constexpr uint32_t kNone = 0xFFFFFFFFu;  // No code point (before start / at end).

/** A set of program counters with O(1) insert, membership and clear. */
class StateSet {
 public:
  void init(size_t n) {
    dense_.resize(n);
    sparse_.resize(n);
    size_ = 0;
  }
  bool contains(uint16_t pc) const {
    const uint16_t i = sparse_[pc];
    return i < size_ && dense_[i] == pc;
  }
  void insert(uint16_t pc) {
    sparse_[pc] = size_;
    dense_[size_++] = pc;
  }
  void clear() { size_ = 0; }
  uint16_t size() const { return size_; }
  uint16_t operator[](uint16_t i) const { return dense_[i]; }

 private:
  std::vector<uint16_t> dense_, sparse_;
  uint16_t size_ = 0;
};

struct Level {
  StateSet sets[2];
  std::vector<uint16_t> stack;
};

/** A position in the input with its neighbouring code points. */
struct Cursor {
  size_t byte = 0;
  uint32_t prev = kNone;
};

bool is_word(uint32_t cp) {
  return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
         cp == '_';
}

class Matcher {
 public:
  Matcher(const Program& program, std::string_view input) : p_(program), input_(input) {
    levels_.resize(static_cast<size_t>(program.look_depth) + 1);
  }

  bool search() { return run(0, Cursor{}, 0, /*anchored=*/false); }

 private:
  uint32_t peek(size_t byte) const {
    if (byte >= input_.size()) return kNone;
    return decode_utf8(input_, byte);
  }

  bool in_ranges(const Inst& in, uint32_t cp) const {
    const Range* lo = p_.ranges.data() + in.a;
    const Range* hi = lo + in.b;
    while (lo < hi) {
      const Range* mid = lo + (hi - lo) / 2;
      if (cp < mid->lo) {
        hi = mid;
      } else if (cp > mid->hi) {
        lo = mid + 1;
      } else {
        return true;
      }
    }
    return false;
  }

  bool assertion(uint8_t kind, const Cursor& at) const {
    switch (kind) {
      case kAnchorStart: return at.byte == 0;
      case kAnchorEnd: return at.byte >= input_.size();
      case kWordBoundary: return is_word(at.prev) != is_word(peek(at.byte));
      case kNotWordBoundary: return is_word(at.prev) == is_word(peek(at.byte));
      default: return false;
    }
  }

  /**
   * Adds `start` and every state reachable from it without consuming input
   * to `set`. Returns true if a Match state is reachable.
   */
  bool closure(Level& level, StateSet& set, int start, const Cursor& at, int depth) {
    std::vector<uint16_t>& stack = level.stack;
    stack.clear();
    auto push = [&](int pc) {
      const uint16_t u = static_cast<uint16_t>(pc);
      if (!set.contains(u)) {
        set.insert(u);
        stack.push_back(u);
      }
    };
    push(start);
    while (!stack.empty()) {
      const int pc = stack.back();
      stack.pop_back();
      const Inst& in = p_.code[pc];
      switch (in.op) {
        case Op::Char:
          break;  // Advanced by the step loop.
        case Op::Match:
          return true;
        case Op::Jmp:
          push(pc + in.a);
          break;
        case Op::Split:
          push(pc + in.a);
          push(pc + in.b);
          break;
        case Op::Assert:
          if (assertion(in.flag, at)) push(pc + 1);
          break;
        case Op::Look:
          if (run(pc + 1, at, depth + 1, /*anchored=*/true) != (in.flag != 0)) push(pc + in.a);
          break;
      }
    }
    return false;
  }

  /**
   * Simulates the program from `start` at `at`. Anchored runs (lookahead
   * bodies) start one thread; unanchored runs start a thread at every
   * position, which gives search semantics.
   */
  bool run(int start, Cursor at, int depth, bool anchored) {
    Level& level = levels_[static_cast<size_t>(depth)];
    if (level.stack.capacity() == 0) {
      level.sets[0].init(p_.code.size());
      level.sets[1].init(p_.code.size());
      level.stack.reserve(p_.code.size());
    }
    StateSet* cur = &level.sets[0];
    StateSet* nxt = &level.sets[1];
    cur->clear();
    if (closure(level, *cur, start, at, depth)) return true;
    for (;;) {
      if (at.byte >= input_.size()) return false;
      if (anchored && cur->size() == 0) return false;
      Cursor after = at;
      const uint32_t c = decode_utf8(input_, after.byte);
      after.prev = c;
      nxt->clear();
      for (uint16_t i = 0; i < cur->size(); ++i) {
        const int pc = (*cur)[i];
        const Inst& in = p_.code[pc];
        if (in.op == Op::Char && in_ranges(in, c) &&
            closure(level, *nxt, pc + 1, after, depth)) {
          return true;
        }
      }
      if (!anchored && closure(level, *nxt, start, after, depth)) return true;
      std::swap(cur, nxt);
      at = after;
    }
  }

  const Program& p_;
  std::string_view input_;
  std::vector<Level> levels_;
};

}  // namespace

Pattern::Pattern() = default;
Pattern::~Pattern() = default;
Pattern::Pattern(Pattern&&) noexcept = default;
Pattern& Pattern::operator=(Pattern&&) noexcept = default;

Result<Pattern> Pattern::compile(std::string_view pattern) {
  auto program = std::make_unique<Program>();
  const CompileResult r = compile_program(pattern, *program);
  if (r.rejection != Rejection::None) {
    std::string message = "Invalid or unsafe regex pattern (";
    message += rejection_token(r.rejection);
    message += "): ";
    message += r.detail;
    if (r.rejection != Rejection::PatternTooLong) {
      message += ": ";
      message += pattern;
    }
    return make_error(ErrorKind::Expression, std::move(message));
  }
  Pattern p;
  p.program_ = std::move(program);
  return Result<Pattern>(std::move(p));
}

bool Pattern::search(std::string_view value) const {
  if (!program_) return false;
  if (utf8_length(value) > kMaxValueCodePoints) return false;
  return Matcher(*program_, value).search();
}

}  // namespace regex
}  // namespace a2ui

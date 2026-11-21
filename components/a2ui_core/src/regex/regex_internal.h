// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Internal representation of a compiled A2UI regex: a Thompson-style NFA
// program run by a Pike VM (regex_vm.cpp). Shared by the compiler
// (regex_compiler.cpp) and the matcher; not part of the public API.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "a2ui/regex.h"

namespace a2ui {
namespace regex {

/**
 * Upper bound on compiled program size. Counted repetitions are expanded
 * (`x{2,5}` emits x five times), so this caps both the program and the VM's
 * per-search scratch (about 10 bytes per instruction per lookahead level).
 * Patterns that expand beyond it are rejected as PATTERN_TOO_LONG.
 */
constexpr size_t kMaxProgramSize = 4096;

/** Lookaheads nest at most this deep (the VM recurses once per level). */
constexpr int kMaxLookaheadNesting = 16;

struct Range {
  uint32_t lo;
  uint32_t hi;
};

enum class Op : uint8_t {
  Char,    // Consume one code point in ranges[a, a + b).
  Split,   // Continue at pc + a and pc + b.
  Jmp,     // Continue at pc + a.
  Assert,  // Zero-width: flag is an Anchor; continue at pc + 1 if it holds.
  Look,    // Lookahead body at pc + 1 (ends in Match); flag = negated; continue at pc + a.
  Match,
};

enum Anchor : uint8_t { kAnchorStart, kAnchorEnd, kWordBoundary, kNotWordBoundary };

struct Inst {
  Op op;
  uint8_t flag;
  int16_t a;   // Relative jump, or range offset for Char.
  uint16_t b;  // Relative jump for Split, or range count for Char.
};

class Program {
 public:
  std::vector<Inst> code;
  std::vector<Range> ranges;
  int look_depth = 0;  // Deepest lookahead nesting in the pattern.
};

/** Why a pattern was rejected; the token names match the other SDKs. */
enum class Rejection : uint8_t {
  None,
  UnsupportedSyntax,
  PatternTooLong,
  NestedQuantifier,
  OverlappingAlternation,
  AdjacentOverlap,
};

const char* rejection_token(Rejection r);

struct CompileResult {
  Rejection rejection = Rejection::None;
  std::string detail;  // Human-readable reason when rejected.
};

/** Parses, safety-checks and compiles `pattern` into `out`. */
CompileResult compile_program(std::string_view pattern, Program& out);

}  // namespace regex
}  // namespace a2ui

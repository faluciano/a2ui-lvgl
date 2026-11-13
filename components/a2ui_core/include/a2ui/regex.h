// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// The A2UI regex dialect: a portable, ReDoS-safe subset of ECMAScript
// patterns in Unicode mode. See "The regex function" in
// specification/v1_0/docs/a2ui_protocol.md and the `regex` section of
// catalogs/basic/v1/basic_catalog_implementation_guide.md.
//
// Patterns are compiled once into a small program and matched with search
// semantics (RegExp.prototype.test). No exceptions, no recursion proportional
// to input length.

#pragma once

#include <memory>
#include <string_view>

#include "a2ui/error.h"

namespace a2ui {
namespace regex {

constexpr size_t kMaxPatternCodePoints = 256;
constexpr size_t kMaxValueCodePoints = 4096;

class Program;  // Opaque compiled pattern.

class Pattern {
 public:
  Pattern();
  ~Pattern();
  Pattern(Pattern&&) noexcept;
  Pattern& operator=(Pattern&&) noexcept;

  /**
   * Parses and safety-checks a pattern. Unsupported syntax and patterns that
   * break the dialect's safety rules fail with ErrorKind::Expression.
   */
  static Result<Pattern> compile(std::string_view pattern);

  /** Search semantics: true if any substring of `value` (UTF-8) matches. */
  bool search(std::string_view value) const;

 private:
  std::unique_ptr<Program> program_;
};

}  // namespace regex
}  // namespace a2ui

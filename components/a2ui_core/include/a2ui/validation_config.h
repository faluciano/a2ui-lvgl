// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// ValidationConfig (blueprint section C): how strictly the message processor
// checks component payloads and the component graph. Supplying no config
// (std::nullopt) skips schema, composition and topology checks entirely.

#pragma once

#include <optional>

namespace a2ui {

struct ValidationConfig {
  bool allow_orphan_components = false;
  bool allow_dangling_references = false;
  bool allow_missing_root = false;
  /** Unknown component types, undeclared properties and undeclared functions. */
  bool allow_unknown_elements = false;
  /** Component tree depth limit; unset means the global limit (50). */
  std::optional<int> max_depth;

  static ValidationConfig strict() { return {}; }
  static ValidationConfig relaxed() {
    ValidationConfig c;
    c.allow_orphan_components = true;
    c.allow_dangling_references = true;
    c.allow_missing_root = true;
    c.allow_unknown_elements = true;
    return c;
  }

  friend bool operator==(const ValidationConfig& a, const ValidationConfig& b) {
    return a.allow_orphan_components == b.allow_orphan_components &&
           a.allow_dangling_references == b.allow_dangling_references &&
           a.allow_missing_root == b.allow_missing_root &&
           a.allow_unknown_elements == b.allow_unknown_elements && a.max_depth == b.max_depth;
  }
  friend bool operator!=(const ValidationConfig& a, const ValidationConfig& b) { return !(a == b); }
};

/** Limits shared by the adapter and the graph checks. */
constexpr int kMaxGlobalDepth = 50;
constexpr int kMaxFunctionCallDepth = 5;

}  // namespace a2ui

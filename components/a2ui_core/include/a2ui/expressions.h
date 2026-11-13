// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// The protocol-version-agnostic `${...}` template parser used by formatString.
// See specification/v1_0/docs/a2ui_protocol.md ("The formatString function")
// and conformance/core/expressions.yaml.

#pragma once

#include <string_view>

#include "a2ui/error.h"
#include "a2ui/json.h"

namespace a2ui {
namespace expressions {

/** Limits that keep parsing bounded on small devices (match the other SDKs). */
struct ParseLimits {
  size_t max_template_length = 0;  // 0 = the SDK default
  size_t max_parts = 0;            // 0 = the SDK default
  int max_depth = 0;               // 0 = the SDK default
};

/**
 * Parses a template into its parts, in the version-neutral shape that
 * conformance/core/expressions.yaml expects:
 *   - literal text and literal values: a string, number, bool or null
 *   - data binding: {"path": "<pointer>"}
 *   - function call: {"call": "<name>", "args": {...}, "returnType": "any"}
 * Errors are ParseError or ExpressionError as the suite specifies.
 */
Result<Json> parse_template(std::string_view text, const ParseLimits& limits = {});

}  // namespace expressions
}  // namespace a2ui

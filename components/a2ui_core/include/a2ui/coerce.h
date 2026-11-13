// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Type coercion standards (blueprint section E, "Type Coercion Standards").

#pragma once

#include <string>

#include "a2ui/json.h"

namespace a2ui {

/** DynamicBoolean coercion: "true"/"false" strings, non-zero numbers, ValidationResult.valid. */
bool coerce_bool(const Json& v);
/** DynamicString coercion: locale-neutral text; null/undefined become "". */
std::string coerce_string(const Json& v);
/** DynamicNumber coercion: numeric strings parse, everything else is 0. */
double coerce_number(const Json& v);
/** True if `v` is a ValidationResult object (an object with a boolean `valid`). */
bool is_validation_result(const Json& v);

}  // namespace a2ui

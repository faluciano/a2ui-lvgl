// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Shared internals of the locale formatters: locale lookup and a small
// decimal type that rounds the way ECMA-402 Intl does (shortest round-trip
// digits of the double, then half-expand), which also feeds plural operands.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "locale_tables.h"

namespace a2ui {
namespace locale {
namespace detail {

/** The table entry for a tag: exact tag, then language, then "en". */
const LocaleData& find_locale(std::string_view tag);

/** `tag` if non-empty, else the default locale. */
std::string_view effective_tag(std::string_view tag);

/** A finite decimal: 0.d1d2..dn x 10^exponent, sign kept separately. */
struct Decimal {
  bool negative = false;
  int ndigits = 0;   // 0 means the value is zero.
  int exponent = 0;  // Number of integer digits when positive.
  char digits[20] = {};

  /** Shortest digits that round-trip to `v` (v must be finite). */
  static Decimal from_double(double v);

  /** Rounds half away from zero to at most `max_fraction` fraction digits. */
  void round_to(int max_fraction);

  /** Integer part digits ("0" for zero) and fraction digits (trailing zeros dropped). */
  std::string integer_digits() const;
  std::string fraction_digits() const;
};

/** First / last code point of a UTF-8 string (0 when empty or malformed). */
uint32_t first_code_point(std::string_view s);
uint32_t last_code_point(std::string_view s);

}  // namespace detail
}  // namespace locale
}  // namespace a2ui

// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Locale-aware formatting without ICU, for the basic catalog functions
// formatNumber, formatCurrency, formatDate and pluralize. A compiled-in table
// of CLDR data covers the locales the device ships; unknown locales fall
// back to the language, then "en". Behaviour follows the other A2UI SDKs
// (Intl in TypeScript, Babel in Python) and conformance/core/functions.yaml;
// where those two disagree this follows Intl (see docs in each function).
//
// Errors: every failure here is an input the reference SDKs render as an
// empty string (ErrorKind::Expression, code "FORMAT_ERROR"); callers
// implementing the basic catalog map a failed Result to "".

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "a2ui/error.h"

namespace a2ui {
namespace locale {

/** The active locale for formatting (BCP 47 tag, e.g. "en-US", "de"). */
void set_default_locale(std::string_view tag);
const std::string& default_locale();

/** The table locale a tag resolves to ("de-AT" -> "de", "xx" -> "en"). */
const char* resolved_locale(std::string_view tag = {});

struct NumberOptions {
  std::optional<int> decimals;  // Forces min = max fraction digits when set.
  bool grouping = true;
  /**
   * With grouping on, honour CLDR minimumGroupingDigits (es, it, pl: "1234" but
   * "12.345"). This is Intl's useGrouping "auto", which applies when the
   * caller passed no `grouping`; Intl treats an explicit `grouping: true` as
   * "always", so set this to false in that case.
   */
  bool locale_min_grouping = true;
};

/**
 * formatNumber: locale grouping and decimal separators. Without `decimals`,
 * 0-3 fraction digits. Rounds the shortest round-trip decimal of `value`
 * half away from zero (Intl; Babel rounds half-even). NaN formats as "" and
 * infinities as "∞"; negative zero keeps its sign ("-0"), as Intl does.
 */
Result<std::string> format_number(double value, const NumberOptions& options,
                                  std::string_view locale_tag = {});

/**
 * formatCurrency: the locale's currency pattern and symbol for an ISO 4217
 * code (case-insensitive), with CLDR currencySpacing ("CHF 1.00"). Like the
 * TypeScript and Python SDKs, `decimals` defaults to 2 for every currency;
 * pass currency_digits(code) to get ISO minor units instead. A code the table
 * does not know, well-formed or not, stands in as its own symbol.
 */
Result<std::string> format_currency(double value, std::string_view currency,
                                    const NumberOptions& options,
                                    std::string_view locale_tag = {});

/** ISO 4217 minor units for a currency code (JPY 0, KWD 3); 2 when unknown. */
int currency_digits(std::string_view currency);

/**
 * formatDate: parses `value` as an ISO 8601 date or date-time
 * (`yyyy-MM-dd`, optionally `[T ]HH:mm[:ss[.fff]]` and `Z`/`±HH[[:]mm]`;
 * years may carry a sign and 4-6 digits) and formats it with a Unicode TR35
 * pattern. Fields format in the offset the value was written with (no offset
 * reads as UTC), never the host zone. Impossible dates and time-only values
 * fail, as in all reference SDKs.
 *
 * The pattern "ISO" yields the UTC instant as JavaScript's toISOString; an
 * empty pattern means "yyyy-MM-dd". Supported fields: y M L d D E c a h H K
 * k m s S z Z O x X with TR35 widths ('M'/'E' are the format forms, 'L'/'c'
 * the stand-alone ones), and '…' quoted literals. Day period 'a' is always
 * AM/PM, as in every reference SDK. Other letters are copied through.
 */
Result<std::string> format_date(std::string_view value_iso8601, std::string_view tr35_pattern,
                                std::string_view locale_tag = {});
/** As format_date, for a UTC instant in epoch milliseconds (|ms| <= 8.64e15). */
Result<std::string> format_date_millis(double epoch_millis, std::string_view tr35_pattern,
                                       std::string_view locale_tag = {});

/**
 * CLDR plural category for a number: "zero", "one", "two", "few", "many" or
 * "other". Like Intl.PluralRules, the operands come from the number rounded
 * to at most 3 fraction digits; the sign is ignored.
 */
const char* plural_category(double value, std::string_view locale_tag = {});

/** Bits for the forms a pluralize call supplies (`zero`, `one`, ... `other`). */
enum PluralFormBits : unsigned {
  kPluralZero = 1u << 0,
  kPluralOne = 1u << 1,
  kPluralTwo = 1u << 2,
  kPluralFew = 1u << 3,
  kPluralMany = 1u << 4,
  kPluralOther = 1u << 5,
};

/**
 * The pluralize argument to render, as all reference SDKs pick it: an exact
 * 0, 1 or 2 selects a supplied `zero`, `one` or `two` regardless of the
 * locale's rules; otherwise the CLDR category if supplied, else "other".
 */
const char* pluralize_key(double value, unsigned present_forms, std::string_view locale_tag = {});

}  // namespace locale
}  // namespace a2ui

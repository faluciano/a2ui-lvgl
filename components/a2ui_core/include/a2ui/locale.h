// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Locale-aware formatting without ICU, for the basic catalog functions
// formatNumber, formatCurrency, formatDate and pluralize. A compiled-in table
// of CLDR data covers the locales the device ships; unknown locales fall
// back to "en". Behaviour follows the other A2UI SDKs (Intl in TypeScript,
// Babel in Python) and conformance/core/functions.yaml.

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

struct NumberOptions {
  std::optional<int> decimals;  // Forces min = max fraction digits when set.
  bool grouping = true;
};

/** formatNumber: locale grouping and decimal separators. */
Result<std::string> format_number(double value, const NumberOptions& options,
                                  std::string_view locale_tag = {});

/** formatCurrency: ISO 4217 code, symbol placement and default digits from CLDR. */
Result<std::string> format_currency(double value, std::string_view currency,
                                    const NumberOptions& options,
                                    std::string_view locale_tag = {});

/**
 * formatDate: parses `value` (ISO 8601 date, date-time or time; or a number
 * of epoch milliseconds) and formats it with a Unicode TR35 pattern.
 */
Result<std::string> format_date(std::string_view value_iso8601, std::string_view tr35_pattern,
                                std::string_view locale_tag = {});
Result<std::string> format_date_millis(double epoch_millis, std::string_view tr35_pattern,
                                       std::string_view locale_tag = {});

/** CLDR plural category for a number: "zero", "one", "two", "few", "many" or "other". */
const char* plural_category(double value, std::string_view locale_tag = {});

}  // namespace locale
}  // namespace a2ui

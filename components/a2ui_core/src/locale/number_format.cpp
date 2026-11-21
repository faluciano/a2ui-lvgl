// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// formatNumber and formatCurrency.

#include <cmath>
#include <cstring>

#include "a2ui/locale.h"
#include "locale_internal.h"

namespace a2ui {
namespace locale {

namespace {

using detail::LocaleData;
using detail::pool_str;

constexpr char kCurrencySign[] = "\xC2\xA4";  // U+00A4, the pattern placeholder.
constexpr char kNbsp[] = "\xC2\xA0";
constexpr char kInfinity[] = "\xE2\x88\x9E";

int clamp_decimals(int decimals) { return decimals < 0 ? 0 : (decimals > 20 ? 20 : decimals); }

/** Digits of |value| with separators, e.g. "1.234,50" (no sign). */
std::string format_magnitude(double value, int min_fraction, int max_fraction, bool grouping,
                             bool min_grouping, const LocaleData& loc) {
  if (std::isinf(value)) return kInfinity;
  detail::Decimal d = detail::Decimal::from_double(value);
  d.round_to(max_fraction);
  const std::string integer = d.integer_digits();
  std::string fraction = d.fraction_digits();
  if (static_cast<int>(fraction.size()) < min_fraction) {
    fraction.append(static_cast<size_t>(min_fraction) - fraction.size(), '0');
  }

  std::string out;
  const size_t n = integer.size();
  const size_t threshold = 3 + (min_grouping ? loc.min_grouping : 1);
  const bool group = grouping && n >= threshold;
  const char* separator = pool_str(loc.group);
  for (size_t i = 0; i < n; ++i) {
    if (group && i > 0 && (n - i) % 3 == 0) out += separator;
    out.push_back(integer[i]);
  }
  if (!fraction.empty()) {
    out += pool_str(loc.decimal);
    out += fraction;
  }
  return out;
}

/** True if `d` rounds to a negative or negative-zero display (Intl shows "-0"). */
bool shows_minus(double value) { return std::signbit(value); }

/** CLDR [[:S:][:Z:]] approximation for code points of caller-supplied codes. */
bool is_symbol_or_space(uint32_t cp) {
  switch (cp) {
    case '$': case '+': case '<': case '=': case '>': case '^': case '`': case '|': case '~':
    case ' ': case 0xA0: case 0xA8: case 0xA9: case 0xAC: case 0xAE: case 0xAF: case 0xB0:
    case 0xB1: case 0xB4: case 0xB8: case 0xD7: case 0xF7: case 0x202F: case 0x205F:
    case 0x3000: case 0xFE69: case 0xFF04:
      return true;
    default:
      break;
  }
  return (cp >= 0xA2 && cp <= 0xA6) || (cp >= 0x2000 && cp <= 0x200A) ||
         (cp >= 0x20A0 && cp <= 0x20C0) || (cp >= 0xFFE0 && cp <= 0xFFE6);
}

struct Symbol {
  std::string text;
  bool first_is_symbol;
  bool last_is_symbol;
};

const detail::CurrencyInfo* find_currency(const char code[3], uint16_t* index) {
  int lo = 0;
  int hi = detail::kCurrencyCount - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    const int cmp = std::memcmp(detail::kCurrencies[mid].code, code, 3);
    if (cmp == 0) {
      *index = static_cast<uint16_t>(mid);
      return &detail::kCurrencies[mid];
    }
    if (cmp < 0) lo = mid + 1;
    else hi = mid - 1;
  }
  return nullptr;
}

std::string upper(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  return out;
}

Symbol currency_symbol(const std::string& code, const LocaleData& loc) {
  uint16_t index = 0;
  const detail::CurrencyInfo* info = code.size() == 3 ? find_currency(code.data(), &index) : nullptr;
  int symbol = info ? info->symbol : 255;
  if (info) {
    for (uint16_t i = 0; i < loc.overrides_count; ++i) {
      const auto& o = detail::kCurrencyOverrides[loc.overrides_start + i];
      if (o.currency == index) {
        symbol = o.symbol;
        break;
      }
    }
  }
  if (symbol == 255) {
    return {code, is_symbol_or_space(detail::first_code_point(code)),
            is_symbol_or_space(detail::last_code_point(code))};
  }
  const auto& s = detail::kCurrencySymbols[symbol];
  return {pool_str(s.text), (s.flags & 1) != 0, (s.flags & 2) != 0};
}

/**
 * Expands a currency pattern affix: "¤" becomes the symbol and "-" the minus
 * sign. Applies CLDR currencySpacing where the symbol touches the digits
 * (`prefix` true: the affix precedes the number).
 */
void append_affix(std::string& out, const char* affix, bool prefix, const Symbol& symbol,
                  const char* minus, bool finite) {
  const size_t sign_len = sizeof(kCurrencySign) - 1;
  const size_t len = std::strlen(affix);
  for (size_t i = 0; i < len;) {
    if (std::strncmp(affix + i, kCurrencySign, sign_len) == 0) {
      const bool at_number = prefix ? (i + sign_len == len) : (i == 0);
      if (finite && at_number && !symbol.text.empty() && !prefix && !symbol.first_is_symbol) {
        out += kNbsp;
      }
      out += symbol.text;
      if (finite && at_number && !symbol.text.empty() && prefix && !symbol.last_is_symbol) {
        out += kNbsp;
      }
      i += sign_len;
    } else if (affix[i] == '-') {
      out += minus;
      ++i;
    } else {
      out.push_back(affix[i++]);
    }
  }
}

}  // namespace

Result<std::string> format_number(double value, const NumberOptions& options,
                                  std::string_view locale_tag) {
  if (std::isnan(value)) return std::string();
  const LocaleData& loc = detail::find_locale(locale_tag);
  const int min_fraction = options.decimals ? clamp_decimals(*options.decimals) : 0;
  const int max_fraction = options.decimals ? clamp_decimals(*options.decimals) : 3;
  std::string out;
  if (shows_minus(value)) out += pool_str(loc.minus);
  out += format_magnitude(std::fabs(value), min_fraction, max_fraction, options.grouping,
                          options.locale_min_grouping, loc);
  return out;
}

Result<std::string> format_currency(double value, std::string_view currency,
                                    const NumberOptions& options, std::string_view locale_tag) {
  if (std::isnan(value)) return std::string();
  const LocaleData& loc = detail::find_locale(locale_tag);
  const int decimals = options.decimals ? clamp_decimals(*options.decimals) : 2;
  const Symbol symbol = currency_symbol(upper(currency), loc);
  const bool negative = shows_minus(value);
  const bool finite = std::isfinite(value);
  const char* minus = pool_str(loc.minus);

  std::string out;
  append_affix(out, pool_str(loc.currency_affix[negative ? 2 : 0]), true, symbol, minus, finite);
  out += format_magnitude(std::fabs(value), decimals, decimals, options.grouping,
                          options.locale_min_grouping, loc);
  append_affix(out, pool_str(loc.currency_affix[negative ? 3 : 1]), false, symbol, minus, finite);
  return out;
}

int currency_digits(std::string_view currency) {
  const std::string code = upper(currency);
  uint16_t index = 0;
  const detail::CurrencyInfo* info = code.size() == 3 ? find_currency(code.data(), &index) : nullptr;
  return info ? info->digits : 2;
}

}  // namespace locale
}  // namespace a2ui

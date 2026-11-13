// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Locale lookup, the shared Decimal type and CLDR plural rules.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "a2ui/locale.h"
#include "locale_internal.h"

namespace a2ui {
namespace locale {

namespace {

std::string& default_tag() {
  static std::string tag = "en-US";
  return tag;
}

char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
  }
  return true;
}

const detail::LocaleTag* find_tag(std::string_view tag) {
  for (uint8_t i = 0; i < detail::kLocaleTagCount; ++i) {
    if (iequals(detail::pool_str(detail::kLocaleTags[i].tag), tag)) return &detail::kLocaleTags[i];
  }
  return nullptr;
}

/** Splits a BCP 47 tag (or a POSIX-style "de_DE") into language and region. */
const detail::LocaleTag* lookup(std::string_view tag) {
  std::string_view subtags[4];
  size_t count = 0;
  size_t start = 0;
  for (size_t i = 0; i <= tag.size() && count < 4; ++i) {
    if (i == tag.size() || tag[i] == '-' || tag[i] == '_') {
      subtags[count++] = tag.substr(start, i - start);
      start = i + 1;
    }
  }
  if (count == 0) return nullptr;
  std::string_view language = subtags[0];
  if (language.size() < 2 || language.size() > 3) return nullptr;
  for (char c : language) {
    if (!is_alpha(c)) return nullptr;
  }
  std::string_view region;
  for (size_t i = 1; i < count && region.empty(); ++i) {
    std::string_view s = subtags[i];
    if (s.size() == 4) continue;  // Script ("Hant").
    if ((s.size() == 2 && is_alpha(s[0]) && is_alpha(s[1])) ||
        (s.size() == 3 && is_digit(s[0]) && is_digit(s[1]) && is_digit(s[2]))) {
      region = s;
    }
    break;
  }
  if (!region.empty()) {
    char buf[8];
    std::memcpy(buf, language.data(), language.size());
    buf[language.size()] = '-';
    std::memcpy(buf + language.size() + 1, region.data(), region.size());
    if (const auto* t = find_tag(std::string_view(buf, language.size() + 1 + region.size()))) {
      return t;
    }
  }
  return find_tag(language);
}

const detail::LocaleTag& resolve(std::string_view tag) {
  if (const auto* t = lookup(detail::effective_tag(tag))) return *t;
  return *find_tag("en");
}

// ---------------------------------------------------------------------------
// Plural rules

struct Operands {
  double n = 0;          // Absolute value.
  std::string n_int;     // Integer digits of n.
  std::string i;         // Integer digits (ICU: the low 18 of them).
  std::string fraction;  // Visible fraction digits (v = size, f = value).
};

/** Value of digit string `digits` modulo `mod` (0 = no modulus), saturating. */
double digits_value(const std::string& digits, uint32_t mod) {
  if (mod != 0) {
    uint64_t r = 0;
    for (char c : digits) r = (r * 10 + static_cast<uint64_t>(c - '0')) % mod;
    return static_cast<double>(r);
  }
  double r = 0;
  for (char c : digits) r = r * 10 + (c - '0');
  return r;
}

uint32_t read_varint(const uint8_t*& p) {
  uint32_t v = 0;
  int shift = 0;
  for (;;) {
    uint8_t b = *p++;
    v |= static_cast<uint32_t>(b & 0x7F) << shift;
    if (!(b & 0x80)) return v;
    shift += 7;
  }
}

bool eval_relation(const uint8_t*& p, const Operands& ops) {
  const uint8_t head = *p++;
  const int operand = head & 7;
  const bool negated = (head & 8) != 0;
  const uint32_t mod = (head & 16) ? read_varint(p) : 0;
  double value = 0;
  bool integral = true;
  switch (operand) {
    case 0: {  // n
      value = digits_value(ops.n_int, mod);
      if (!ops.fraction.empty()) {
        integral = false;
        value += std::fmod(ops.n, 1.0);
      }
      break;
    }
    case 1: value = digits_value(ops.i, mod); break;                                  // i
    case 2: case 3: value = static_cast<double>(ops.fraction.size()); break;          // v, w
    case 4: case 5: value = digits_value(ops.fraction, mod); break;                   // f, t
    default: value = 0; break;                                                        // e, c
  }
  if (mod != 0 && (operand == 2 || operand == 3)) value = std::fmod(value, mod);
  const uint8_t nranges = *p++;
  bool match = false;
  for (uint8_t r = 0; r < nranges; ++r) {
    const uint32_t lo = read_varint(p);
    const uint32_t hi = read_varint(p);
    if (integral && value >= lo && value <= hi) match = true;
  }
  return match != negated;
}

const char* const kCategories[] = {"zero", "one", "two", "few", "many", "other"};

int plural_index(double value, std::string_view tag) {
  if (!std::isfinite(value)) return 5;
  detail::Decimal d = detail::Decimal::from_double(std::fabs(value));
  d.negative = false;
  d.round_to(3);
  Operands ops;
  ops.i = d.integer_digits();
  ops.n_int = ops.i;
  // ICU keeps the low 18 integer digits when i overflows int64 (1e21 -> 0).
  if (ops.i.size() > 18) {
    ops.i.erase(0, ops.i.size() - 18);
    const size_t nonzero = ops.i.find_first_not_of('0');
    ops.i.erase(0, nonzero == std::string::npos ? ops.i.size() - 1 : nonzero);
  }
  ops.fraction = d.fraction_digits();
  // n is the rounded absolute value, as Intl computes operands after rounding.
  ops.n = std::strtod((ops.i + "." + (ops.fraction.empty() ? "0" : ops.fraction)).c_str(), nullptr);

  const uint8_t* p = detail::kPluralCode + detail::find_locale(tag).plural;
  while (*p != 0xFF) {
    const uint8_t category = *p++;
    const uint8_t n_or = *p++;
    bool any = false;
    for (uint8_t o = 0; o < n_or; ++o) {
      const uint8_t n_and = *p++;
      bool all = true;
      for (uint8_t a = 0; a < n_and; ++a) {
        // Evaluate every relation so `p` advances past each one.
        if (!eval_relation(p, ops)) all = false;
      }
      if (all) any = true;
    }
    if (any) return category;
  }
  return 5;
}

}  // namespace

// ---------------------------------------------------------------------------
// detail

namespace detail {

std::string_view effective_tag(std::string_view tag) {
  return tag.empty() ? std::string_view(default_tag()) : tag;
}

const LocaleData& find_locale(std::string_view tag) { return kLocales[resolve(tag).locale]; }

Decimal Decimal::from_double(double v) {
  Decimal d;
  d.negative = std::signbit(v);
  if (v == 0) return d;
  char buf[40];
  for (int precision = 1; precision <= 17; ++precision) {
    std::snprintf(buf, sizeof buf, "%.*e", precision - 1, v);
    if (std::strtod(buf, nullptr) == v) break;
  }
  // buf is "[-]d[.ddd]e[+-]xx".
  const char* p = buf;
  if (*p == '-') ++p;
  for (; *p && *p != 'e'; ++p) {
    if (*p != '.' && d.ndigits < 17) d.digits[d.ndigits++] = *p;
  }
  while (d.ndigits > 0 && d.digits[d.ndigits - 1] == '0') --d.ndigits;
  d.exponent = (*p == 'e' ? std::atoi(p + 1) : 0) + 1;
  return d;
}

void Decimal::round_to(int max_fraction) {
  if (ndigits == 0) return;
  const int keep = exponent + max_fraction;  // Digits left of the rounding point.
  if (keep >= ndigits) return;
  if (keep < 0) {
    ndigits = 0;
    exponent = 0;
    return;
  }
  const bool up = digits[keep] >= '5';
  ndigits = keep;
  if (up) {
    int i = keep - 1;
    while (i >= 0 && digits[i] == '9') --i;
    if (i < 0) {
      digits[0] = '1';
      ndigits = 1;
      exponent += 1;
    } else {
      digits[i] += 1;
      ndigits = i + 1;
    }
  }
  while (ndigits > 0 && digits[ndigits - 1] == '0') --ndigits;
  if (ndigits == 0) exponent = 0;
}

std::string Decimal::integer_digits() const {
  if (ndigits == 0 || exponent <= 0) return "0";
  std::string out(digits, static_cast<size_t>(std::min(ndigits, exponent)));
  if (exponent > ndigits) out.append(static_cast<size_t>(exponent - ndigits), '0');
  return out;
}

std::string Decimal::fraction_digits() const {
  if (ndigits <= exponent || ndigits == 0) return {};
  std::string out;
  if (exponent < 0) out.append(static_cast<size_t>(-exponent), '0');
  const int start = exponent > 0 ? exponent : 0;
  out.append(digits + start, static_cast<size_t>(ndigits - start));
  return out;
}

uint32_t first_code_point(std::string_view s) {
  if (s.empty()) return 0;
  const auto* p = reinterpret_cast<const unsigned char*>(s.data());
  int len = p[0] < 0x80 ? 1 : (p[0] >> 5) == 6 ? 2 : (p[0] >> 4) == 14 ? 3 : (p[0] >> 3) == 30 ? 4 : 0;
  if (len == 0 || static_cast<size_t>(len) > s.size()) return 0;
  if (len == 1) return p[0];
  uint32_t cp = p[0] & (0x7F >> len);
  for (int i = 1; i < len; ++i) cp = (cp << 6) | (p[i] & 0x3F);
  return cp;
}

uint32_t last_code_point(std::string_view s) {
  size_t i = s.size();
  while (i > 0 && (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0x80) --i;
  if (i == 0) return 0;
  return first_code_point(s.substr(i - 1));
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Public API

void set_default_locale(std::string_view tag) { default_tag().assign(tag.data(), tag.size()); }

const std::string& default_locale() { return default_tag(); }

const char* resolved_locale(std::string_view tag) { return detail::pool_str(resolve(tag).tag); }

const char* plural_category(double value, std::string_view locale_tag) {
  return kCategories[plural_index(value, locale_tag)];
}

const char* pluralize_key(double value, unsigned present, std::string_view locale_tag) {
  if (value == 0 && (present & kPluralZero)) return "zero";
  if (value == 1 && (present & kPluralOne)) return "one";
  if (value == 2 && (present & kPluralTwo)) return "two";
  const int category = plural_index(value, locale_tag);
  if (present & (1u << category)) return kCategories[category];
  return "other";
}

}  // namespace locale
}  // namespace a2ui

// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Locale formatting: hand-written cases from the reference SDKs' own tests
// (TypeScript web_core, Python a2ui_core), a golden grid generated from Intl
// (tools/gen_intl_golden.mjs), and every formatting case in the conformance
// suites, evaluated by calling the locale API directly.

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

#include "a2ui/json.h"
#include "a2ui/locale.h"
#include "test_main.h"

namespace loc = a2ui::locale;
using a2ui::Json;

namespace {

std::string num(double v, std::optional<int> decimals = std::nullopt, const char* locale = "en-US",
                int grouping = -1) {
  loc::NumberOptions o;
  o.decimals = decimals;
  if (grouping >= 0) {
    o.grouping = grouping == 1;
    o.locale_min_grouping = false;
  }
  auto r = loc::format_number(v, o, locale);
  return r.ok() ? r.value() : "<error>";
}

std::string cur(double v, const char* code, const char* locale = "en-US",
                std::optional<int> decimals = std::nullopt) {
  loc::NumberOptions o;
  o.decimals = decimals;
  auto r = loc::format_currency(v, code, o, locale);
  return r.ok() ? r.value() : "<error>";
}

std::string date(const char* value, const char* pattern, const char* locale = "en-US") {
  auto r = loc::format_date(value, pattern, locale);
  return r.ok() ? r.value() : "";
}

#define EXPECT_STR(actual, expected)                                                   \
  do {                                                                                 \
    const std::string a_ = (actual);                                                   \
    const std::string e_ = (expected);                                                 \
    if (a_ != e_) {                                                                    \
      unit::report_failure(__FILE__, __LINE__,                                         \
                           std::string(#actual) + " = \"" + a_ + "\", want \"" + e_ + "\""); \
    }                                                                                  \
  } while (0)

}  // namespace

// ---------------------------------------------------------------------------
// Cases from the reference SDK tests

TEST(locale_format_number_reference_cases) {
  EXPECT_STR(num(1234.56, 1), "1,234.6");
  EXPECT_STR(num(1234.56, 1, "en-US", 0), "1234.6");
  EXPECT_STR(num(1234.56, 2), "1,234.56");
  EXPECT_STR(num(1234.56, 2, "de-DE"), "1.234,56");
  EXPECT_STR(num(1234.56, 2, "fr-FR"), "1 234,56");
  EXPECT_STR(num(1234.567, 2, "en-US", 1), "1,234.57");
  EXPECT_STR(num(1234.567, 0, "en-US", 0), "1235");
  EXPECT_STR(num(NAN), "");
  for (const char* tag : {"xx-YY", "zz", "en_US", "!!!", "", "not a locale"}) {
    EXPECT_STR(num(1234.56, 2, tag), "1,234.56");
  }
  EXPECT_STR(num(1234.56, 2, "de_DE"), "1.234,56");  // POSIX separator, as Python accepts.
  // Defaults: 0-3 fraction digits, Intl half-expand on the shortest decimal.
  EXPECT_STR(num(1234.5678), "1,234.568");
  EXPECT_STR(num(2.5, 0), "3");
  EXPECT_STR(num(1.005, 2), "1.01");
  EXPECT_STR(num(-0.0), "-0");
  EXPECT_STR(num(1e21), "1,000,000,000,000,000,000,000");
  EXPECT_STR(num(INFINITY), "∞");
}

TEST(locale_min_grouping_follows_intl) {
  EXPECT_STR(num(1234.5, std::nullopt, "es"), "1234,5");       // useGrouping "auto"
  EXPECT_STR(num(12345.5, std::nullopt, "es"), "12.345,5");
  EXPECT_STR(num(1234.5, std::nullopt, "es", 1), "1.234,5");  // explicit grouping: true
  EXPECT_STR(num(1234.5, std::nullopt, "pl"), "1234,5");
}

TEST(locale_format_currency_reference_cases) {
  EXPECT_STR(cur(1234.56, "USD"), "$1,234.56");
  EXPECT_STR(cur(1234.56, "usd"), "$1,234.56");
  EXPECT_STR(cur(99.99, "USD", "en-US", 2), "$99.99");
  EXPECT_STR(cur(1234.5, "USD", "en-US", 2), "$1,234.50");
  EXPECT_STR(cur(1234.56, "EUR", "de-DE"), "1.234,56 €");
  EXPECT_STR(cur(1234.56, "USD", "fr-FR"), "1 234,56 $US");
  // CLDR currencySpacing.
  EXPECT_STR(cur(1234.56, "CHF"), "CHF 1,234.56");
  EXPECT_STR(cur(1234.56, "SEK"), "SEK 1,234.56");
  EXPECT_STR(cur(1234.56, "EUR"), "€1,234.56");
  EXPECT_STR(cur(1234.56, "HKD"), "HK$1,234.56");
  EXPECT_STR(cur(1234.56, "CHF", "de-DE"), "1.234,56 CHF");
  EXPECT_STR(cur(1234.56, "USD", "de-DE"), "1.234,56 $");
  // Codes the table does not know stand in as their own symbol.
  EXPECT_STR(cur(1234.56, "INVALID-CURRENCY"), "INVALID-CURRENCY 1,234.56");
  EXPECT_STR(cur(1234.56, "INVALID-CURRENCY", "de-DE"), "1.234,56 INVALID-CURRENCY");
  EXPECT_STR(cur(1234.56, "INVALID-CURRENCY", "fr-FR"), "1 234,56 INVALID-CURRENCY");
  EXPECT_STR(cur(1234.56, "INVALID-CURRENCY", "ja-JP"), "INVALID-CURRENCY 1,234.56");
  EXPECT_STR(cur(1234.56, "INVALID-CURRENCY", "!!!"), "INVALID-CURRENCY 1,234.56");
  EXPECT_STR(cur(1234.56, "XYZ"), "XYZ 1,234.56");
  EXPECT_STR(cur(1234.56, "XYZ", "de-DE"), "1.234,56 XYZ");
  EXPECT_STR(cur(1234.56, "USD", "ar-EG"), "‏1,234.56 US$");  // Latin digits, as Babel.
  EXPECT_STR(cur(NAN, "USD"), "");
  // Decimals default to 2 for every currency, as in TypeScript and Python.
  EXPECT_STR(cur(1234, "JPY"), "¥1,234.00");
  EXPECT_EQ(loc::currency_digits("JPY"), 0);
  EXPECT_EQ(loc::currency_digits("kwd"), 3);
  EXPECT_EQ(loc::currency_digits("USD"), 2);
  EXPECT_EQ(loc::currency_digits("XYZ"), 2);
  EXPECT_STR(cur(1234, "JPY", "ja", loc::currency_digits("JPY")), "￥1,234");
}

TEST(locale_format_date_reference_cases) {
  const char* sunday = "2025-03-09T07:05:00";
  const char* const tokens[][2] = {
      {"yyyy", "2025"}, {"yy", "25"},     {"MMMM", "March"}, {"MMM", "Mar"}, {"MM", "03"},
      {"M", "3"},       {"EEEE", "Sunday"}, {"E", "Sun"},    {"dd", "09"},   {"d", "9"},
      {"HH", "07"},     {"H", "7"},       {"hh", "07"},      {"h", "7"},     {"mm", "05"},
      {"ss", "00"},     {"a", "AM"},
  };
  for (const auto& t : tokens) EXPECT_STR(date(sunday, t[0]), t[1]);
  EXPECT_STR(date(sunday, "EEEE MMMM d yyyy HH:mm:ss"), "Sunday March 9 2025 07:05:00");
  EXPECT_STR(date("2025-03-09T15:05:00", "hh:mm a"), "03:05 PM");
  EXPECT_STR(date("2025-01-01T23:30:00+05:30", "yyyy-MM-dd HH:mm"), "2025-01-01 23:30");
  EXPECT_STR(date("2025-01-01T23:30:00", "yyyy-MM-dd HH:mm"), "2025-01-01 23:30");
  EXPECT_STR(date("2025-01-01T12:00:00Z", "ISO"), "2025-01-01T12:00:00.000Z");
  EXPECT_STR(date("2025-01-01T12:00:00+02:00", "ISO"), "2025-01-01T10:00:00.000Z");
  EXPECT_STR(date("2025-01-01T12:00:00-05:00", "ISO"), "2025-01-01T17:00:00.000Z");
  EXPECT_STR(date("2025-01-01T12:00:00", "ISO"), "2025-01-01T12:00:00.000Z");
  EXPECT_STR(date("invalid-date", "yyyy"), "");
  EXPECT_STR(date("", "yyyy"), "");
  EXPECT_STR(date(sunday, ""), "2025-03-09");
  EXPECT_STR(date(sunday, "EEEE MMMM", "de-DE"), "Sonntag März");
  EXPECT_STR(date(sunday, "yyyy-MM-dd", "de-DE"), "2025-03-09");
  EXPECT_STR(date("2026-06-10T12:00:00Z", "EEEE, MMMM d, yyyy", "fr-FR"), "mercredi, juin 10, 2026");
  EXPECT_STR(date("2026-06-10T12:00:00Z", "EEEE, MMMM d, yyyy", "de-DE"), "Mittwoch, Juni 10, 2026");
  EXPECT_STR(date("2026-06-10T12:00:00Z", "EEEE, MMMM d, yyyy", "xx-YY"), "Wednesday, June 10, 2026");
  const char* dt = "2026-03-05T08:04:09Z";
  EXPECT_STR(date(dt, "yy-M-d H:mm:ss"), "26-3-5 8:04:09");
  EXPECT_STR(date(dt, "MMM MMMM E EEEE hh:mm:ss a"), "Mar March Thu Thursday 08:04:09 AM");
  EXPECT_STR(date("2026-03-05T15:04:09Z", "h:mm:ss a HH"), "3:04:09 PM 15");
}

TEST(locale_format_date_parsing) {
  EXPECT_STR(date("2026-02-30", "yyyy-MM-dd"), "");
  EXPECT_STR(date("2026-13-01", "yyyy-MM-dd"), "");
  EXPECT_STR(date("2024-02-29", "yyyy-MM-dd"), "2024-02-29");
  EXPECT_STR(date("2023-02-29", "yyyy-MM-dd"), "");
  EXPECT_STR(date("2026-09-04T24:30", "HH:mm"), "");
  EXPECT_STR(date("2026-09-04T23:60", "HH:mm"), "");
  EXPECT_STR(date("2026-09-04T23:59:60", "HH:mm"), "");
  EXPECT_STR(date("12:30:00", "HH:mm"), "");  // Time-only: rejected by every reference SDK.
  EXPECT_STR(date("2026-09-04T12:00:00Z\n", "yyyy"), "");
  EXPECT_STR(date("2026-09-04t12:00:00z", "HH"), "12");
  EXPECT_STR(date("2026-09-04 12:00", "HH:mm"), "12:00");
  EXPECT_STR(date("2026-09-04T12:00:00.9999Z", "ss.SSS"), "00.999");  // Truncated.
  EXPECT_STR(date("2026-09-04T12:00:00,5+0530", "SSSS Z"), "5000 +0530");
  EXPECT_STR(date("2026-09-04T12:00-05", "HH xxx"), "12 -05:00");
  EXPECT_STR(date("2026-09-04T12:00+25:00", "HH"), "");
  EXPECT_STR(date("+012026-09-04", "yyyy"), "12026");
  EXPECT_STR(date("-000001-01-01T00:00:00Z", "ISO"), "-000001-01-01T00:00:00.000Z");
  EXPECT_STR(date("+275760-09-13T00:00:00Z", "ISO"), "+275760-09-13T00:00:00.000Z");
  EXPECT_STR(date("+275760-09-13T00:00:01Z", "ISO"), "");  // Beyond a JavaScript Date.
  EXPECT_STR(date("1969-12-31T23:59:59.999Z", "ISO"), "1969-12-31T23:59:59.999Z");
  EXPECT_STR(date("2026-09-04T00:30:00+01:00", "ISO"), "2026-09-03T23:30:00.000Z");
}

TEST(locale_format_date_tr35) {
  const char* t = "2026-09-04T08:04:09.123-05:30";
  EXPECT_STR(date(t, "yyyy-MM-dd'T'HH:mm:ss.SSSXXX"), "2026-09-04T08:04:09.123-05:30");
  EXPECT_STR(date(t, "'It''s' h 'o''clock'"), "It's 8 o'clock");
  EXPECT_STR(date(t, "''"), "'");
  EXPECT_STR(date(t, "z | zzzz | O | OOOO | Z | ZZZZ | ZZZZZ | X | XX | x"),
             "GMT-5:30 | GMT-05:30 | GMT-5:30 | GMT-05:30 | -0530 | GMT-05:30 | -05:30 | -0530 | -0530 | -0530");
  EXPECT_STR(date("2026-09-04T08:00Z", "X XXX x z ZZZZ ZZZZZ"), "Z Z +00 GMT GMT Z");
  EXPECT_STR(date("2026-09-04T00:00Z", "h K k H"), "12 0 24 0");
  EXPECT_STR(date("2026-12-31", "D DDD"), "365 365");
  EXPECT_STR(date("2026-02-01", "D DDD"), "32 032");
  EXPECT_STR(date("2026-03-09", "MMMMM EEEEE EEE y yyy"), "M M Mon 2026 2026");
  EXPECT_STR(date("2025-03-09", "d MMMM", "ru"), "9 марта");
  EXPECT_STR(date("2025-03-09", "LLLL", "ru"), "март");
  EXPECT_STR(date("2025-03-09", "LLL cccc", "de"), "Mär Sonntag");
  EXPECT_STR(date("2025-03-09", "G q w"), "G q w");  // Unsupported letters pass through.
  auto r = loc::format_date_millis(1767225600000.0, "yyyy-MM-dd HH:mm EEEE");
  EXPECT_TRUE(r.ok());
  EXPECT_STR(r.value(), "2026-01-01 00:00 Thursday");
  EXPECT_STR(loc::format_date_millis(-1, "ISO").value(), "1969-12-31T23:59:59.999Z");
  EXPECT_TRUE(!loc::format_date_millis(NAN, "ISO").ok());
  EXPECT_TRUE(!loc::format_date_millis(8.64e15 + 1, "ISO").ok());
  auto bad = loc::format_date("nope", "yyyy");
  EXPECT_TRUE(!bad.ok() && bad.error().kind == a2ui::ErrorKind::Expression);
}

TEST(locale_plural_reference_cases) {
  EXPECT_STR(loc::plural_category(1), "one");
  EXPECT_STR(loc::plural_category(2), "other");
  EXPECT_STR(loc::plural_category(1.5), "other");
  EXPECT_STR(loc::plural_category(1.0004), "one");  // Intl rounds to 3 fraction digits first.
  EXPECT_STR(loc::plural_category(-1), "one");
  EXPECT_STR(loc::plural_category(INFINITY), "other");
  EXPECT_STR(loc::plural_category(0, "fr"), "one");
  EXPECT_STR(loc::plural_category(1000000, "fr"), "many");
  EXPECT_STR(loc::plural_category(22, "ru"), "few");
  EXPECT_STR(loc::plural_category(1.5, "ru"), "other");
  EXPECT_STR(loc::plural_category(103, "ar"), "few");
  EXPECT_STR(loc::plural_category(0, "ar"), "zero");
  EXPECT_STR(loc::plural_category(5, "ja"), "other");
  // pluralize: explicit 0/1/2 forms win; absent categories fall back to other.
  const unsigned zero_one_other = loc::kPluralZero | loc::kPluralOne | loc::kPluralOther;
  EXPECT_STR(loc::pluralize_key(0, zero_one_other), "zero");
  EXPECT_STR(loc::pluralize_key(1, zero_one_other), "one");
  EXPECT_STR(loc::pluralize_key(2, loc::kPluralTwo | loc::kPluralOther), "two");
  EXPECT_STR(loc::pluralize_key(0, loc::kPluralOther), "other");
  EXPECT_STR(loc::pluralize_key(1, loc::kPluralOther), "other");
  EXPECT_STR(loc::pluralize_key(5, loc::kPluralOne | loc::kPluralOther), "other");
  EXPECT_STR(loc::pluralize_key(3, loc::kPluralFew | loc::kPluralOther, "ru"), "few");
  EXPECT_STR(loc::pluralize_key(3, loc::kPluralOther, "ru"), "other");
}

TEST(locale_resolution) {
  EXPECT_STR(loc::resolved_locale("en-US"), "en");
  EXPECT_STR(loc::resolved_locale("en-GB"), "en-GB");
  EXPECT_STR(loc::resolved_locale("EN_gb"), "en-GB");
  EXPECT_STR(loc::resolved_locale("pt-BR"), "pt-BR");
  EXPECT_STR(loc::resolved_locale("pt-PT"), "pt");
  EXPECT_STR(loc::resolved_locale("zh-Hant-TW"), "zh");
  EXPECT_STR(loc::resolved_locale("de-u-co-phonebk"), "de");
  EXPECT_STR(loc::resolved_locale("xx-YY"), "en");
  EXPECT_STR(loc::resolved_locale("!!!"), "en");
  EXPECT_STR(loc::default_locale(), "en-US");
  EXPECT_STR(loc::resolved_locale("de-AT"), "de");  // Region data is not carried: "1.234", not "1 234".
  loc::set_default_locale("de-DE");
  EXPECT_STR(num(1234.5, std::nullopt, ""), "1.234,5");
  EXPECT_STR(loc::resolved_locale(), "de");
  loc::set_default_locale("en-US");
  EXPECT_STR(num(1234.5, std::nullopt, ""), "1,234.5");
}

// ---------------------------------------------------------------------------
// Golden grid from Intl (Node full ICU)

namespace {

struct GoldenTally {
  int checked = 0;
  int failed = 0;
  void check(bool ok, const std::string& what) {
    ++checked;
    if (ok) return;
    if (++failed <= 25) std::printf("  golden mismatch: %s\n", what.c_str());
  }
};

struct NumberRow {
  const char* locale;
  double value;
  int decimals;  // -1: absent.
  int grouping;  // -1: absent (Intl "auto"), else explicit false/true.
  const char* expected;
};
const NumberRow kNumberRows[] = {
#define GOLDEN_NUMBER(l, v, d, g, e) {l, v, d, g, e},
#include "locale_intl_golden.inc"
#undef GOLDEN_NUMBER
};

struct CurrencyRow {
  const char* locale;
  const char* code;
  double value;
  int decimals;
  int grouping;
  const char* expected;
};
const CurrencyRow kCurrencyRows[] = {
#define GOLDEN_CURRENCY(l, c, v, d, g, e) {l, c, v, d, g, e},
#include "locale_intl_golden.inc"
#undef GOLDEN_CURRENCY
};

struct PluralRow {
  const char* locale;
  double value;
  const char* expected;
};
const PluralRow kPluralRows[] = {
#define GOLDEN_PLURAL(l, v, e) {l, v, e},
#include "locale_intl_golden.inc"
#undef GOLDEN_PLURAL
};

struct NameRow {
  const char* locale;
  const char* iso;
  const char* pattern;
  const char* expected;
};
const NameRow kNameRows[] = {
#define GOLDEN_NAMES(l, i, p, e) {l, i, p, e},
#include "locale_intl_golden.inc"
#undef GOLDEN_NAMES
};

loc::NumberOptions golden_options(int decimals, int grouping) {
  loc::NumberOptions o;
  if (decimals >= 0) o.decimals = decimals;
  if (grouping >= 0) {
    o.grouping = grouping == 1;
    o.locale_min_grouping = false;
  }
  return o;
}

std::string describe(const char* kind, const char* locale, const std::string& input,
                     const std::string& got, const char* want) {
  return std::string(kind) + " " + locale + " " + input + " -> \"" + got + "\" want \"" + want + "\"";
}

}  // namespace

TEST(locale_matches_intl_golden_grid) {
  GoldenTally t;
  for (const auto& r : kNumberRows) {
    const std::string got = loc::format_number(r.value, golden_options(r.decimals, r.grouping),
                                               r.locale).value();
    t.check(got == r.expected,
            describe("number", r.locale,
                     a2ui::number_to_string(r.value) + " d" + std::to_string(r.decimals) + " g" +
                         std::to_string(r.grouping), got, r.expected));
  }
  for (const auto& r : kCurrencyRows) {
    const std::string got = loc::format_currency(r.value, r.code,
                                                 golden_options(r.decimals, r.grouping),
                                                 r.locale).value();
    t.check(got == r.expected,
            describe("currency", r.locale,
                     std::string(r.code) + " " + a2ui::number_to_string(r.value) + " d" +
                         std::to_string(r.decimals) + " g" + std::to_string(r.grouping),
                     got, r.expected));
  }
  for (const auto& r : kPluralRows) {
    const std::string got = loc::plural_category(r.value, r.locale);
    t.check(got == r.expected,
            describe("plural", r.locale, a2ui::number_to_string(r.value), got, r.expected));
  }
  for (const auto& r : kNameRows) {
    const std::string got = date(r.iso, r.pattern, r.locale);
    t.check(got == r.expected,
            describe("names", r.locale, std::string(r.iso) + " " + r.pattern, got, r.expected));
  }
  std::printf("  intl golden grid: %d checked, %d mismatched\n", t.checked, t.failed);
  EXPECT_EQ(t.failed, 0);
}

// ---------------------------------------------------------------------------
// Conformance suites

namespace {

bool read_json(const std::string& path, Json* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  auto r = Json::parse(ss.str());
  if (!r.ok) return false;
  *out = std::move(r.value);
  return true;
}

bool is_formatting_function(const std::string& name) {
  return name == "formatNumber" || name == "formatCurrency" || name == "formatDate" ||
         name == "pluralize";
}

/** True if every argument is a literal (no data binding or nested call). */
bool literal_args(const Json& args) {
  for (const auto& m : args.as_object()) {
    if (m.second.is_container()) return false;
  }
  return true;
}

/**
 * The basic-catalog glue the reference SDKs put around these functions,
 * reduced to argument unpacking: every formatting failure renders "".
 */
std::string evaluate(const std::string& fn, const Json& args) {
  const Json& value = args["value"];
  loc::NumberOptions o;
  if (args["decimals"].is_number()) o.decimals = static_cast<int>(args["decimals"].as_number());
  if (args["grouping"].is_bool()) {
    o.grouping = args["grouping"].as_bool();
    o.locale_min_grouping = false;  // Intl: an explicit grouping is "always".
  }
  if (fn == "formatNumber") {
    auto r = loc::format_number(value.as_number(NAN), o);
    return r.ok() ? r.value() : "";
  }
  if (fn == "formatCurrency") {
    auto r = loc::format_currency(value.as_number(NAN), args["currency"].as_string(), o);
    return r.ok() ? r.value() : "";
  }
  if (fn == "formatDate") {
    if (!value.is_string()) return "";
    const Json& format = args["format"];
    auto r = loc::format_date(value.as_string(), format.is_string() ? format.as_string() : "");
    return r.ok() ? r.value() : "";
  }
  // pluralize
  static const char* const kForms[] = {"zero", "one", "two", "few", "many", "other"};
  unsigned present = 0;
  for (int i = 0; i < 6; ++i) {
    if (args.contains(kForms[i])) present |= 1u << i;
  }
  const Json& form = args[loc::pluralize_key(value.as_number(NAN), present)];
  return form.is_string() ? form.as_string() : "";
}

struct SuiteTally {
  int checked = 0;
  std::vector<std::string> skipped;
};

void check_case(const std::string& name, const std::string& fn, const Json& args,
                const Json& expect, SuiteTally* tally) {
  if (!literal_args(args)) {
    tally->skipped.push_back(name + " (argument resolves through the catalog)");
    return;
  }
  ++tally->checked;
  const std::string got = evaluate(fn, args);
  if (!expect.is_string() || got != expect.as_string()) {
    unit::report_failure(__FILE__, __LINE__,
                         name + ": " + fn + " -> \"" + got + "\", want " + expect.dump());
  }
}

/** Formatting calls in a `validate` case's components, checked against expect.surfaces. */
void scan_validate_case(const Json& c, SuiteTally* tally) {
  const std::string name = c["name"].as_string();
  if (!c["steps"].is_array()) return;
  for (const Json& step : c["steps"].as_array()) {
    const bool expects_error = step.contains("expectError");
    if (!step["payload"].is_array()) continue;
    for (const Json& msg : step["payload"].as_array()) {
      const Json* body = msg.find("createSurface");
      if (!body) body = msg.find("updateComponents");
      if (!body || !(*body)["components"].is_array()) continue;
      const std::string surface = (*body)["surfaceId"].as_string();
      for (const Json& comp : (*body)["components"].as_array()) {
        for (const auto& prop : comp.as_object()) {
          const Json& call = prop.second;
          if (!call.is_object() || !call["@call"].is_string()) continue;
          const std::string fn = call["@call"].as_string();
          if (!is_formatting_function(fn)) continue;
          if (expects_error) {
            tally->skipped.push_back(name + " (expects a catalog schema ValidationError)");
            continue;
          }
          const Json& expect = c["expect"]["surfaces"][surface]["components"]
                                [comp["id"].as_string()][prop.first];
          if (expect.is_undefined()) continue;
          check_case(name, fn, call["args"], expect, tally);
        }
      }
    }
  }
}

}  // namespace

TEST(locale_conformance_formatting_cases) {
  Json index;
  const std::string dir = "build/conformance/core/";
  if (!read_json(dir + "functions.json", &index)) {
    unit::report_failure(__FILE__, __LINE__, "missing " + dir + "functions.json (run make suites)");
    return;
  }
  SuiteTally tally;
  for (const Json& c : index.as_array()) {
    const std::string name = c["name"].as_string();
    const std::string action = c["action"].as_string();
    if (action == "evaluate_function") {
      const std::string fn = c["function"].as_string();
      if (!is_formatting_function(fn)) {
        if (c["args"].dump().find("pluralize(") != std::string::npos ||
            c["args"].dump().find("formatNumber(") != std::string::npos) {
          tally.skipped.push_back(name + " (formatString template: catalog resolution)");
        }
        continue;
      }
      if (c.contains("expectError")) {
        tally.skipped.push_back(name + " (expectError)");
        continue;
      }
      check_case(name, fn, c["args"], c["expect"], &tally);
    } else if (action == "validate") {
      scan_validate_case(c, &tally);
    }
  }
  // Other suites that render formatting calls with literal arguments.
  for (const char* suite : {"multi_catalog", "expressions", "message_processor_v1_0",
                            "validator_v1_0", "rpc_functions", "agent_function_routing"}) {
    Json cases;
    if (!read_json(dir + suite + ".json", &cases) || !cases.is_array()) continue;
    for (const Json& c : cases.as_array()) {
      if (c["action"].as_string() == "validate") scan_validate_case(c, &tally);
    }
  }
  std::printf("  conformance formatting cases: %d checked, %zu skipped\n", tally.checked,
              tally.skipped.size());
  for (const auto& s : tally.skipped) std::printf("    skipped: %s\n", s.c_str());
  EXPECT_TRUE(tally.checked >= 17);
}

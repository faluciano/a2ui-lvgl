// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// formatDate: ISO 8601 parsing and Unicode TR35 pattern formatting.

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "a2ui/locale.h"
#include "locale_internal.h"

namespace a2ui {
namespace locale {

namespace {

using detail::LocaleData;
using detail::pool_str;

/** The largest |epoch ms| a JavaScript Date holds (100,000,000 days). */
constexpr double kMaxEpochMillis = 8.64e15;
constexpr int64_t kMillisPerDay = 86400000;

Error format_error(std::string message) {
  return make_error(ErrorKind::Expression, std::move(message), "FORMAT_ERROR");
}

/** Wall-clock fields plus the instant they denote. */
struct DateTime {
  int64_t year = 1970;
  int month = 1, day = 1, hour = 0, minute = 0, second = 0, millis = 0;
  int offset_seconds = 0;
  int64_t epoch_millis = 0;
};

// Howard Hinnant's civil-from-days algorithms (proleptic Gregorian).
int64_t days_from_civil(int64_t y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void civil_from_days(int64_t z, int64_t* y, int* m, int* d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  *d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  *m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  *y = yoe + era * 400 + (*m <= 2);
}

bool is_leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int days_in_month(int64_t y, int m) {
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && is_leap(y) ? 29 : kDays[m - 1];
}

int64_t floor_div(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

DateTime from_epoch(int64_t ms, int offset_seconds) {
  DateTime t;
  t.epoch_millis = ms;
  t.offset_seconds = offset_seconds;
  const int64_t local = ms + static_cast<int64_t>(offset_seconds) * 1000;
  const int64_t days = floor_div(local, kMillisPerDay);
  int64_t in_day = local - days * kMillisPerDay;
  civil_from_days(days, &t.year, &t.month, &t.day);
  t.hour = static_cast<int>(in_day / 3600000);
  in_day %= 3600000;
  t.minute = static_cast<int>(in_day / 60000);
  in_day %= 60000;
  t.second = static_cast<int>(in_day / 1000);
  t.millis = static_cast<int>(in_day % 1000);
  return t;
}

// ---------------------------------------------------------------------------
// ISO 8601 parsing

class Scanner {
 public:
  explicit Scanner(std::string_view s) : s_(s) {}
  bool done() const { return i_ == s_.size(); }
  char peek() const { return done() ? '\0' : s_[i_]; }
  bool accept(char c) {
    if (peek() != c) return false;
    ++i_;
    return true;
  }
  /** Reads between `min` and `max` ASCII digits. */
  bool digits(int min, int max, int64_t* out, int* count = nullptr) {
    int n = 0;
    int64_t v = 0;
    while (n < max && !done() && s_[i_] >= '0' && s_[i_] <= '9') {
      v = v * 10 + (s_[i_++] - '0');
      ++n;
    }
    if (count) *count = n;
    *out = v;
    return n >= min;
  }
  /** Reads a run of digits of any length, keeping the first three as millis. */
  bool fraction(int* millis) {
    int n = 0;
    int v = 0;
    while (!done() && s_[i_] >= '0' && s_[i_] <= '9') {
      if (n < 3) v = v * 10 + (s_[i_] - '0');
      ++i_;
      ++n;
    }
    for (int k = n; k < 3; ++k) v *= 10;
    *millis = v;
    return n > 0;
  }

 private:
  std::string_view s_;
  size_t i_ = 0;
};

bool parse_iso(std::string_view text, DateTime* out) {
  Scanner sc(text);
  int64_t year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  int millis = 0;
  int year_digits = 0;
  int sign = 1;
  if (sc.accept('-')) sign = -1;
  else sc.accept('+');
  if (!sc.digits(4, 6, &year, &year_digits)) return false;
  year *= sign;
  if (!sc.accept('-') || !sc.digits(2, 2, &month) || !sc.accept('-') || !sc.digits(2, 2, &day)) {
    return false;
  }
  int offset = 0;
  if (!sc.done()) {
    if (!(sc.accept('T') || sc.accept('t') || sc.accept(' '))) return false;
    if (!sc.digits(2, 2, &hour) || !sc.accept(':') || !sc.digits(2, 2, &minute)) return false;
    if (sc.accept(':')) {
      if (!sc.digits(2, 2, &second)) return false;
      if ((sc.accept('.') || sc.accept(',')) && !sc.fraction(&millis)) return false;
    }
    if (sc.accept('Z') || sc.accept('z')) {
      // UTC.
    } else if (sc.peek() == '+' || sc.peek() == '-') {
      const int offset_sign = sc.accept('-') ? -1 : (sc.accept('+'), 1);
      int64_t oh = 0, om = 0;
      if (!sc.digits(2, 2, &oh)) return false;
      if (!sc.done()) {
        sc.accept(':');
        if (!sc.digits(2, 2, &om)) return false;
      }
      if (oh >= 24 || om >= 60) return false;
      offset = offset_sign * static_cast<int>(oh * 3600 + om * 60);
    }
    if (!sc.done()) return false;
  }
  if (month < 1 || month > 12 || day < 1 || day > days_in_month(year, static_cast<int>(month)) ||
      hour > 23 || minute > 59 || second > 59) {
    return false;
  }
  const int64_t days = days_from_civil(year, static_cast<int>(month), static_cast<int>(day));
  const int64_t ms = days * kMillisPerDay + hour * 3600000 + minute * 60000 + second * 1000 + millis -
                     static_cast<int64_t>(offset) * 1000;
  if (std::fabs(static_cast<double>(ms)) > kMaxEpochMillis) return false;
  *out = from_epoch(ms, offset);
  return true;
}

// ---------------------------------------------------------------------------
// Formatting

void append_padded(std::string& out, int64_t value, int width) {
  char buf[32];
  if (value < 0) {
    out.push_back('-');
    value = -value;
  }
  std::snprintf(buf, sizeof buf, "%0*lld", width, static_cast<long long>(value));
  out += buf;
}

std::string iso_string(const DateTime& utc) {
  std::string out;
  if (utc.year >= 0 && utc.year <= 9999) {
    append_padded(out, utc.year, 4);
  } else {
    out.push_back(utc.year < 0 ? '-' : '+');
    append_padded(out, utc.year < 0 ? -utc.year : utc.year, 6);
  }
  char buf[40];
  std::snprintf(buf, sizeof buf, "-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.month, utc.day, utc.hour,
                utc.minute, utc.second, utc.millis);
  return out + buf;
}

/** "+05:30", "-05", "+0530" ... `colon`, `force_minutes`; "Z" handled by callers. */
void append_offset(std::string& out, int offset_seconds, bool colon, bool force_minutes) {
  const int abs = offset_seconds < 0 ? -offset_seconds : offset_seconds;
  out.push_back(offset_seconds < 0 ? '-' : '+');
  append_padded(out, abs / 3600, 2);
  const int minutes = (abs % 3600) / 60;
  if (force_minutes || minutes != 0) {
    if (colon) out.push_back(':');
    append_padded(out, minutes, 2);
  }
}

/** Localized GMT format: "GMT", "GMT-5", "GMT+5:30" (short) or "GMT-05:00" (long). */
void append_gmt(std::string& out, int offset_seconds, bool long_form) {
  out += "GMT";
  if (offset_seconds == 0) return;
  const int abs = offset_seconds < 0 ? -offset_seconds : offset_seconds;
  out.push_back(offset_seconds < 0 ? '-' : '+');
  append_padded(out, abs / 3600, long_form ? 2 : 1);
  const int minutes = (abs % 3600) / 60;
  if (long_form || minutes != 0) {
    out.push_back(':');
    append_padded(out, minutes, 2);
  }
}

const char* name(const uint16_t (*lists)[12], uint16_t list, int index) {
  return pool_str(lists[list][index]);
}

/** Expands one run of pattern letter `c` repeated `n` times; false if unsupported. */
bool expand_field(std::string& out, char c, int n, const DateTime& t, const LocaleData& loc) {
  using namespace detail;
  const int64_t days = days_from_civil(t.year, t.month, t.day);
  const int weekday = static_cast<int>(((days % 7) + 11) % 7);  // 0 = Sunday (1970-01-01 = Thu).
  switch (c) {
    case 'y':
      if (n == 2) {
        append_padded(out, (t.year < 0 ? -t.year : t.year) % 100, 2);
      } else {
        append_padded(out, t.year, n);
      }
      return true;
    case 'M':
    case 'L': {
      if (n <= 2) {
        append_padded(out, t.month, n);
        return true;
      }
      const int base = c == 'M' ? kFormatWide : kStandAloneWide;
      const int width = n == 3 ? 1 : (n == 4 ? 0 : 2);  // abbr, wide, narrow
      out += name(kNames12, loc.months[base + width], t.month - 1);
      return true;
    }
    case 'd':
      if (n > 2) return false;
      append_padded(out, t.day, n);
      return true;
    case 'D':
      if (n > 3) return false;
      append_padded(out, days - days_from_civil(t.year, 1, 1) + 1, n);
      return true;
    case 'E':
    case 'c':
    case 'e': {
      if (c != 'E' && n <= 2) return false;  // Numeric local day of week: unsupported.
      const int base = c == 'c' ? kStandAloneWide : kFormatWide;
      const int width = n == 4 ? 0 : (n == 5 ? 2 : 1);  // wide, narrow, else abbr
      out += pool_str(kNames7[loc.days[base + width]][weekday]);
      return true;
    }
    case 'a':
      out += t.hour < 12 ? "AM" : "PM";
      return true;
    case 'h':
      if (n > 2) return false;
      append_padded(out, t.hour % 12 == 0 ? 12 : t.hour % 12, n);
      return true;
    case 'H':
      if (n > 2) return false;
      append_padded(out, t.hour, n);
      return true;
    case 'K':
      if (n > 2) return false;
      append_padded(out, t.hour % 12, n);
      return true;
    case 'k':
      if (n > 2) return false;
      append_padded(out, t.hour == 0 ? 24 : t.hour, n);
      return true;
    case 'm':
      if (n > 2) return false;
      append_padded(out, t.minute, n);
      return true;
    case 's':
      if (n > 2) return false;
      append_padded(out, t.second, n);
      return true;
    case 'S': {
      char buf[4];
      std::snprintf(buf, sizeof buf, "%03d", t.millis);
      for (int i = 0; i < n; ++i) out.push_back(i < 3 ? buf[i] : '0');
      return true;
    }
    case 'Z':
      if (n <= 3) {
        append_offset(out, t.offset_seconds, false, true);
      } else if (n == 4) {
        append_gmt(out, t.offset_seconds, true);
      } else if (t.offset_seconds == 0) {
        out.push_back('Z');
      } else {
        append_offset(out, t.offset_seconds, true, true);
      }
      return true;
    case 'X':
    case 'x':
      if (n > 5) return false;
      if (c == 'X' && t.offset_seconds == 0) {
        out.push_back('Z');
      } else {
        append_offset(out, t.offset_seconds, n == 3 || n == 5, n != 1);
      }
      return true;
    case 'O':
      if (n != 1 && n != 4) return false;
      append_gmt(out, t.offset_seconds, n == 4);
      return true;
    case 'z':
      if (n > 4) return false;
      append_gmt(out, t.offset_seconds, n == 4);
      return true;
    default:
      return false;
  }
}

bool is_pattern_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

std::string format_fields(const DateTime& t, std::string_view pattern, const LocaleData& loc) {
  std::string out;
  size_t i = 0;
  while (i < pattern.size()) {
    const char c = pattern[i];
    if (c == '\'') {
      if (i + 1 < pattern.size() && pattern[i + 1] == '\'') {  // '' is a literal quote.
        out.push_back('\'');
        i += 2;
        continue;
      }
      // Quoted literal; '' inside it is a quote. An unterminated quote runs to the end.
      for (++i; i < pattern.size(); ++i) {
        if (pattern[i] == '\'') {
          if (i + 1 < pattern.size() && pattern[i + 1] == '\'') {
            out.push_back('\'');
            ++i;
            continue;
          }
          ++i;
          break;
        }
        out.push_back(pattern[i]);
      }
      continue;
    }
    if (!is_pattern_letter(c)) {
      out.push_back(c);
      ++i;
      continue;
    }
    size_t j = i;
    while (j < pattern.size() && pattern[j] == c) ++j;
    const int n = static_cast<int>(j - i);
    if (!expand_field(out, c, n, t, loc)) out.append(pattern.substr(i, j - i));
    i = j;
  }
  return out;
}

Result<std::string> format(const DateTime& t, std::string_view pattern, std::string_view locale_tag) {
  if (pattern == "ISO") return iso_string(from_epoch(t.epoch_millis, 0));
  if (pattern.empty()) pattern = "yyyy-MM-dd";
  return format_fields(t, pattern, detail::find_locale(locale_tag));
}

}  // namespace

Result<std::string> format_date(std::string_view value, std::string_view pattern,
                                std::string_view locale_tag) {
  DateTime t;
  if (!parse_iso(value, &t)) return format_error("formatDate: not an ISO 8601 date");
  return format(t, pattern, locale_tag);
}

Result<std::string> format_date_millis(double epoch_millis, std::string_view pattern,
                                       std::string_view locale_tag) {
  if (!std::isfinite(epoch_millis) || std::fabs(epoch_millis) > kMaxEpochMillis) {
    return format_error("formatDate: time value out of range");
  }
  return format(from_epoch(static_cast<int64_t>(std::floor(epoch_millis)), 0), pattern, locale_tag);
}

}  // namespace locale
}  // namespace a2ui

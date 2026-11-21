// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// ISO 8601 values of DateTimeInput: "yyyy-MM-dd", "HH:mm[:ss]" and
// "yyyy-MM-ddTHH:mm[:ss[.fff]][Z|±HH:mm|±HHmm|±HH]".
//
// The fields are wall-clock values: a microcontroller usually has no time
// zone database, so a zone designator is kept verbatim and written back
// unchanged, never converted. A picked value keeps the shape of the previous
// value (seconds, zone) when it had one, as the SwiftUI renderer does.

#pragma once

#include <string>

namespace a2ui_lvgl {
namespace basic {

struct IsoDateTime {
  bool has_date = false;
  bool has_time = false;
  int year = 0, month = 0, day = 0;
  int hour = 0, minute = 0, second = 0;
  bool has_seconds = false;
  /** Fraction digits after '.', kept verbatim. */
  std::string fraction;
  /** "Z", "+02:00", "-0500", "+02", or empty (local time). */
  std::string zone;
};

/** Parses a date, time or date-time; false for anything else or out-of-range fields. */
bool parse_iso8601(const std::string& text, IsoDateTime* out);

/**
 * Formats `value` for a DateTimeInput with `enable_date` / `enable_time`
 * (neither: date only), in the shape of `previous` (may be null).
 */
std::string format_iso8601(const IsoDateTime& value, bool enable_date, bool enable_time,
                           const IsoDateTime* previous);

/** Days in a month of the proleptic Gregorian calendar. */
int days_in_month(int year, int month);

}  // namespace basic
}  // namespace a2ui_lvgl

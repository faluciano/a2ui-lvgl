// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0

#include "a2ui/identifier.h"

#include <cstddef>

namespace a2ui {
namespace {

#include "xid_tables.inc"

template <size_t N>
bool in_table(const uint32_t (&table)[N], uint32_t cp) {
  // Find the last entry whose start is <= cp.
  size_t lo = 0, hi = N;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if ((table[mid] >> 11) <= cp) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == 0) return false;
  const uint32_t e = table[lo - 1];
  return cp - (e >> 11) <= (e & 0x7FFu);
}

bool is_ascii_alpha(uint32_t cp) { return (cp | 0x20u) >= 'a' && (cp | 0x20u) <= 'z'; }

}  // namespace

bool is_xid_start(uint32_t cp) {
  if (cp < 0x80) return is_ascii_alpha(cp);
  return in_table(kXidStart, cp);
}

bool is_xid_continue(uint32_t cp) {
  if (cp < 0x80) return is_ascii_alpha(cp) || (cp >= '0' && cp <= '9') || cp == '_';
  return in_table(kXidContinue, cp);
}

uint32_t decode_utf8(std::string_view s, size_t& pos) {
  const size_t n = s.size();
  const auto byte = [&](size_t i) { return static_cast<uint8_t>(s[i]); };
  const uint8_t b0 = byte(pos);
  if (b0 < 0x80) {
    ++pos;
    return b0;
  }
  size_t len;
  uint32_t cp, min;
  if ((b0 & 0xE0) == 0xC0) {
    len = 2, cp = b0 & 0x1F, min = 0x80;
  } else if ((b0 & 0xF0) == 0xE0) {
    len = 3, cp = b0 & 0x0F, min = 0x800;
  } else if ((b0 & 0xF8) == 0xF0) {
    len = 4, cp = b0 & 0x07, min = 0x10000;
  } else {
    ++pos;
    return 0xFFFD;
  }
  if (pos + len > n) {
    ++pos;
    return 0xFFFD;
  }
  for (size_t i = 1; i < len; ++i) {
    const uint8_t b = byte(pos + i);
    if ((b & 0xC0) != 0x80) {
      ++pos;
      return 0xFFFD;
    }
    cp = (cp << 6) | (b & 0x3F);
  }
  if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    ++pos;
    return 0xFFFD;
  }
  pos += len;
  return cp;
}

bool utf8_valid(std::string_view s) {
  size_t pos = 0;
  while (pos < s.size()) {
    const size_t before = pos;
    if (decode_utf8(s, pos) == 0xFFFD && pos - before != 3) return false;
  }
  return true;
}

size_t utf8_length(std::string_view s) {
  size_t count = 0;
  for (size_t pos = 0; pos < s.size(); ++count) decode_utf8(s, pos);
  return count;
}

bool is_uax31_identifier(std::string_view utf8) {
  if (utf8.empty() || !utf8_valid(utf8)) return false;
  size_t pos = 0;
  const uint32_t first = decode_utf8(utf8, pos);
  if (first != '_' && !is_xid_start(first)) return false;
  while (pos < utf8.size()) {
    if (!is_xid_continue(decode_utf8(utf8, pos))) return false;
  }
  return true;
}

}  // namespace a2ui

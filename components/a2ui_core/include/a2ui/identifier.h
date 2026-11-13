// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Unicode UAX #31 identifiers (^[\p{XID_Start}_][\p{XID_Continue}]*$), which
// v1.0 requires for component types, function names, property and argument
// names and extension keys.

#pragma once

#include <cstdint>
#include <string_view>

namespace a2ui {

bool is_xid_start(uint32_t code_point);
bool is_xid_continue(uint32_t code_point);

/** True if `utf8` is a non-empty, valid UTF-8 UAX #31 identifier. */
bool is_uax31_identifier(std::string_view utf8);

/**
 * Decodes one UTF-8 code point at `pos`, advancing it. Returns 0xFFFD and
 * advances one byte for malformed input.
 */
uint32_t decode_utf8(std::string_view s, size_t& pos);

/** Number of code points in a UTF-8 string. */
size_t utf8_length(std::string_view s);

}  // namespace a2ui

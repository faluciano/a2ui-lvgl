// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Minimal PNG encoder (8-bit RGB, stored deflate blocks, no compression
// library needed). Files are larger than a real encoder's, which is fine
// for a debugging tool.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace png_writer {

/** Writes `rgb` (width*height*3 bytes, row-major) to `path`. Returns false on I/O failure. */
bool write_rgb(const std::string& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgb);

}  // namespace png_writer

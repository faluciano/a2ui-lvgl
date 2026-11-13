// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0

#include "png_writer.h"

#include <cstdio>

namespace png_writer {

namespace {

uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc = 0) {
  static uint32_t table[256];
  static bool made = false;
  if (!made) {
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[n] = c;
    }
    made = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void put32(std::vector<uint8_t>* out, uint32_t v) {
  out->push_back(static_cast<uint8_t>(v >> 24));
  out->push_back(static_cast<uint8_t>(v >> 16));
  out->push_back(static_cast<uint8_t>(v >> 8));
  out->push_back(static_cast<uint8_t>(v));
}

void chunk(std::vector<uint8_t>* out, const char* type, const std::vector<uint8_t>& data) {
  put32(out, static_cast<uint32_t>(data.size()));
  std::vector<uint8_t> body(type, type + 4);
  body.insert(body.end(), data.begin(), data.end());
  out->insert(out->end(), body.begin(), body.end());
  put32(out, crc32(body.data(), body.size()));
}

}  // namespace

bool write_rgb(const std::string& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgb) {
  // Raw scanlines with filter byte 0.
  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(height) * (width * 3 + 1));
  for (uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);
    const uint8_t* row = rgb.data() + static_cast<size_t>(y) * width * 3;
    raw.insert(raw.end(), row, row + static_cast<size_t>(width) * 3);
  }
  // zlib stream of stored deflate blocks.
  std::vector<uint8_t> z = {0x78, 0x01};
  size_t pos = 0;
  do {
    const size_t n = raw.size() - pos > 65535 ? 65535 : raw.size() - pos;
    const bool last = pos + n == raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(static_cast<uint8_t>(n & 0xFF));
    z.push_back(static_cast<uint8_t>(n >> 8));
    z.push_back(static_cast<uint8_t>(~n & 0xFF));
    z.push_back(static_cast<uint8_t>((~n >> 8) & 0xFF));
    z.insert(z.end(), raw.begin() + static_cast<long>(pos), raw.begin() + static_cast<long>(pos + n));
    pos += n;
  } while (pos < raw.size());
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  put32(&z, (b << 16) | a);

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  put32(&ihdr, width);
  put32(&ihdr, height);
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit, RGB, deflate, no filter, no interlace.
  chunk(&png, "IHDR", ihdr);
  chunk(&png, "IDAT", z);
  chunk(&png, "IEND", {});

  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
  return std::fclose(f) == 0 && ok;
}

}  // namespace png_writer

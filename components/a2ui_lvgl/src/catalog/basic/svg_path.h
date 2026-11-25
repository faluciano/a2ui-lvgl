// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// A small SVG path renderer for Icon `svgPath` names: parses path data
// (M L H V C S Q T A Z, absolute and relative) on the 24 x 24 view box the
// other A2UI renderers use, and fills it (nonzero rule, 4x vertical
// supersampling with exact horizontal coverage) into an 8-bit alpha mask.
// The mask is drawn as an LV_COLOR_FORMAT_A8 image tinted with the text
// colour, so no LVGL vector backend is needed.

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace a2ui_lvgl {
namespace basic {

/** Path data longer than this is not rendered. */
constexpr size_t kMaxSvgPathLength = 8192;
/** Flattened edges kept at most (about 16 bytes each while rasterizing). */
constexpr size_t kMaxSvgEdges = 4096;

/**
 * Renders `path` (view box 0 0 24 24) into a `size` x `size` alpha mask
 * (row-major, stride `size`). Path data is drawn up to its first error, as
 * SVG prescribes. Returns false when nothing could be drawn (empty or invalid
 * from the start, too long), leaving `out` empty.
 */
bool render_svg_path(std::string_view path, int size, std::vector<uint8_t>* out);

}  // namespace basic
}  // namespace a2ui_lvgl

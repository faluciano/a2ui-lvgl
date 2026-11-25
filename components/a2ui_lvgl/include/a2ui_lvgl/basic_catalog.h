// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// LVGL implementations of the A2UI v1.0 basic catalog components
// (catalogs/basic/v1, basic_catalog_implementation_guide.md).
//
// Decoupled from the surface and dispatcher: an application can start from
// basic_registry(), copy it, and replace any entry (ComponentRegistry::add)
// with its own design-system widget.
//
// Microcontroller notes:
//   - Video and AudioPlayer render nothing (no media pipeline on an MCU);
//     register your own implementation to show a player or a poster.
//   - Image pixels come from SurfaceConfig::services.image_loader; without
//     one, a placeholder box sized by the variant is shown.
//   - Icon maps names onto LV_SYMBOL_* glyphs of the built-in fonts, with a
//     text fallback; `svgPath` icons are filled into an alpha mask.
//   - Text renders a Markdown subset (markdown.h).

#pragma once

#include "a2ui_lvgl/component.h"

namespace a2ui_lvgl {

/** Adds the basic catalog implementations (catalog-agnostic entries) to `registry`. */
void register_basic_components(ComponentRegistry& registry);

/** A process-wide registry holding the basic catalog implementations. */
const ComponentRegistry& basic_registry();

/** The LV_SYMBOL_* (or fallback) glyph the Icon component shows for a basic catalog icon name. */
const char* basic_icon_glyph(const std::string& name);

}  // namespace a2ui_lvgl

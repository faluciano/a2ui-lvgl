// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Presentation defaults for the LVGL adapter.
//
// A2UI v1.0 carries no theme in the protocol (createSurface.theme is gone),
// so the look of a surface is the application's choice. Theme holds the
// colours, fonts and spacing the basic catalog widgets use; the application
// passes one in SurfaceConfig. Following the basic catalog guide:
//   - leaf components and outlined containers carry `leaf_margin` as an
//     external margin; Row, Column and List carry no spacing at all;
//   - text colour is set once on the surface (and on primary buttons) and
//     reaches Text and Icon through LVGL style inheritance; leaves never set
//     a colour of their own (except the muted caption variant);
//   - cards are outlined with a transparent background, so nested cards stay
//     distinct without depth tracking.

#pragma once

#include <cstdint>

#include "lvgl.h"

namespace a2ui_lvgl {

/** Start from Theme::light() or Theme::dark_theme() and adjust. */
struct Theme {
  bool dark = false;

  // Colours.
  lv_color_t background{};       // Surface background.
  lv_color_t on_background{};    // Default text and icon colour (inherited).
  lv_color_t muted{};            // Caption text, placeholders, secondary marks.
  lv_color_t primary{};          // Primary buttons, selection, active tab.
  lv_color_t on_primary{};       // Text on primary backgrounds.
  lv_color_t control{};          // Default button / chip background.
  lv_color_t outline{};          // Card outline, dividers, field borders.
  lv_color_t error{};            // Validation errors.
  lv_color_t warning{};          // Validation warnings.
  lv_color_t info{};             // Validation info hints.
  lv_color_t scrim{};            // Modal backdrop (drawn at `scrim_opa`).
  lv_opa_t scrim_opa = LV_OPA_50;
  lv_color_t dialog{};           // Modal / popup panel background.

  // Fonts. Null entries fall back to `body`.
  const lv_font_t* body = nullptr;
  const lv_font_t* caption = nullptr;
  /** h1..h6 (Markdown headings); index 0 is h1. */
  const lv_font_t* heading[6] = {};
  /** Fonts for Markdown emphasis; null renders the run in the body font. */
  const lv_font_t* bold = nullptr;
  const lv_font_t* italic = nullptr;
  const lv_font_t* code = nullptr;
  /** Font holding the LV_SYMBOL_* glyphs (the built-in Montserrat fonts do). */
  const lv_font_t* icon = nullptr;

  // Spacing (pixels).
  int32_t leaf_margin = 6;
  int32_t card_padding = 6;
  int32_t card_radius = 8;
  int32_t control_radius = 6;
  int32_t button_padding = 4;
  int32_t field_padding = 6;
  /** Height of a vertical divider whose container gives it none. */
  int32_t divider_min_length = 24;

  /** Image box sizes per variant (basic catalog guide). */
  int32_t image_icon = 24;
  int32_t image_avatar = 40;
  int32_t image_small = 100;
  int32_t image_medium = 200;
  int32_t image_large = 300;
  int32_t image_header = 160;

  /** Light palette with the built-in fonts that are enabled in lv_conf. */
  static Theme light();
  static Theme dark_theme();

  const lv_font_t* font_or_body(const lv_font_t* f) const { return f ? f : body; }
};

/**
 * Initializes LVGL's default theme on `display` with the theme's primary
 * colour, dark mode and body font, so stock widgets (sliders, checkboxes,
 * text areas, calendars) match. Optional; call once after creating the display.
 */
void apply_lvgl_theme(lv_display_t* display, const Theme& theme);

}  // namespace a2ui_lvgl

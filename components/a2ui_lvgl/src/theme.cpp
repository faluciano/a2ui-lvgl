// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0

#include "a2ui_lvgl/theme.h"

namespace a2ui_lvgl {

namespace {

void set_default_fonts(Theme* t) {
  t->body = LV_FONT_DEFAULT;
  // Icons read best larger than body text (the guide suggests 24 dp).
#if LV_FONT_MONTSERRAT_24
  t->icon = &lv_font_montserrat_24;
#elif LV_FONT_MONTSERRAT_20
  t->icon = &lv_font_montserrat_20;
#else
  t->icon = LV_FONT_DEFAULT;
#endif
#if LV_FONT_MONTSERRAT_12
  t->caption = &lv_font_montserrat_12;
#endif
#if LV_FONT_MONTSERRAT_24
  t->heading[0] = &lv_font_montserrat_24;
#elif LV_FONT_MONTSERRAT_20
  t->heading[0] = &lv_font_montserrat_20;
#endif
#if LV_FONT_MONTSERRAT_20
  t->heading[1] = &lv_font_montserrat_20;
#endif
#if LV_FONT_MONTSERRAT_16
  t->heading[2] = &lv_font_montserrat_16;
#endif
  // h4..h6 use the body font, as do bold, italic and code: the built-in
  // fonts have no such faces. Applications with richer fonts set them.
}

}  // namespace

Theme Theme::light() {
  Theme t;
  t.dark = false;
  t.background = lv_color_hex(0xFFFFFF);
  t.on_background = lv_color_hex(0x1F1F1F);
  t.muted = lv_color_hex(0x6B6B6B);
  t.primary = lv_color_hex(0x1A73E8);
  t.on_primary = lv_color_hex(0xFFFFFF);
  t.control = lv_color_hex(0xEEF1F5);
  t.outline = lv_color_hex(0xC7CBD1);
  t.error = lv_color_hex(0xC5221F);
  t.warning = lv_color_hex(0xB06000);
  t.info = lv_color_hex(0x1967D2);
  t.scrim = lv_color_hex(0x000000);
  t.dialog = lv_color_hex(0xFFFFFF);
  set_default_fonts(&t);
  return t;
}

Theme Theme::dark_theme() {
  Theme t;
  t.dark = true;
  t.background = lv_color_hex(0x121212);
  t.on_background = lv_color_hex(0xE8EAED);
  t.muted = lv_color_hex(0x9AA0A6);
  t.primary = lv_color_hex(0x8AB4F8);
  t.on_primary = lv_color_hex(0x0B1A33);
  t.control = lv_color_hex(0x2A2D31);
  t.outline = lv_color_hex(0x5F6368);
  t.error = lv_color_hex(0xF28B82);
  t.warning = lv_color_hex(0xFDD663);
  t.info = lv_color_hex(0x8AB4F8);
  t.scrim = lv_color_hex(0x000000);
  t.scrim_opa = LV_OPA_60;
  t.dialog = lv_color_hex(0x202124);
  set_default_fonts(&t);
  return t;
}

void apply_lvgl_theme(lv_display_t* display, const Theme& theme) {
#if LV_USE_THEME_DEFAULT
  lv_theme_t* th = lv_theme_default_init(display, theme.primary, theme.muted, theme.dark, theme.body);
  lv_display_set_theme(display, th);
#else
  (void)display;
  (void)theme;
#endif
}

}  // namespace a2ui_lvgl

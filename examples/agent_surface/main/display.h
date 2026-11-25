// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// The display chosen in menuconfig: an SPI ST7789/ILI9341 panel through
// esp_lcd, or a headless display that drops its pixels. The same code as
// examples/basic_surface.

#pragma once

#include "lvgl.h"

/** Adds the display to esp_lvgl_port (after lvgl_port_init). Null on failure. */
lv_display_t* example_add_display();

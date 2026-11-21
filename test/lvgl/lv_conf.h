// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// LVGL configuration for host builds (smoke test and adapter tests).
// Anything not set here takes the default from lv_conf_internal.h.
//
// - No OS, no SDL/X11/framebuffer: tests drive a display with a memory
//   buffer and a flush callback that discards pixels.
// - The C library allocator is used instead of LVGL's TLSF pool so that
//   AddressSanitizer sees every allocation.

#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565

#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_USE_OS LV_OS_NONE
#define LV_DEF_REFR_PERIOD 33

#define LV_USE_DRAW_SW 1

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_STYLE 1
#define LV_USE_ASSERT_OBJ 1

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_USE_FLEX 1
#define LV_USE_GRID 1
#define LV_USE_THEME_DEFAULT 1
#define LV_USE_OBSERVER 1

#define LV_USE_SDL 0
#define LV_USE_X11 0
#define LV_USE_LINUX_FBDEV 0
#define LV_USE_LINUX_DRM 0

#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0

#endif  // LV_CONF_H

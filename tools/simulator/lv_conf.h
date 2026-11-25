// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// LVGL configuration for the SDL desktop simulator: the host test
// configuration (test/lvgl/lv_conf.h) plus LVGL's SDL window, mouse and
// keyboard drivers. Found first on the include path by `make simulator`.

#ifndef A2UI_SIMULATOR_LV_CONF_H
#define A2UI_SIMULATOR_LV_CONF_H

#include "../../test/lvgl/lv_conf.h"

// 32-bit frames: SDL shows them as is, and --screenshot reads them back.
#undef LV_COLOR_FORMAT_DEFAULT
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_XRGB8888

// stdout carries the messages for the agent; the simulator sends LVGL's log
// to stderr instead. The object and style checks are off for speed.
#undef LV_LOG_PRINTF
#define LV_LOG_PRINTF 0
#undef LV_USE_ASSERT_OBJ
#define LV_USE_ASSERT_OBJ 0
#undef LV_USE_ASSERT_STYLE
#define LV_USE_ASSERT_STYLE 0

#undef LV_USE_SDL
#define LV_USE_SDL 1
#define LV_SDL_INCLUDE_PATH "SDL2/SDL.h"
#define LV_SDL_AUTO_BACKEND 0
#define LV_SDL_BACKEND LV_SDL_BACKEND_SW
#define LV_SDL_RENDER_MODE LV_DISPLAY_RENDER_MODE_DIRECT
#define LV_SDL_BUF_COUNT 1
// A software renderer works everywhere, including SDL_VIDEODRIVER=dummy.
#define LV_SDL_ACCELERATED 0
#define LV_SDL_FULLSCREEN 0
// The simulator handles closing the window itself, so it can clean up.
#define LV_SDL_DIRECT_EXIT 0
#define LV_SDL_MOUSEWHEEL_MODE LV_SDL_MOUSEWHEEL_MODE_ENCODER

#endif  // A2UI_SIMULATOR_LV_CONF_H

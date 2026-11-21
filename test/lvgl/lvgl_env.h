// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Headless LVGL for host tests and tools: a display backed by a memory
// buffer, a fake tick, and helpers to let timers and layout settle.

#pragma once

#include <cstdint>
#include <vector>

#include "lvgl.h"

namespace lvgl_env {

/** lv_init() and the tick source (idempotent). */
void init();
/** lv_deinit() (deletes every display). */
void deinit();

struct Display {
  lv_display_t* display = nullptr;
  int32_t width = 0;
  int32_t height = 0;
  lv_color_format_t format = LV_COLOR_FORMAT_RGB565;
  uint32_t stride = 0;
  /**
   * With full_frame, the whole frame lives in `pixels` (direct render mode)
   * after refresh(); otherwise a partial buffer whose pixels are discarded.
   */
  std::vector<uint8_t> pixels;
};

/** Creates a display (and makes it the default). The Display must outlive the LVGL display. */
void create_display(Display* out, int32_t width, int32_t height, lv_color_format_t format, bool full_frame);

/** Advances the tick by `ms` per pass and runs the timer handler `passes` times. */
void run(int passes = 3, uint32_t ms = 40);
/** Lays out and redraws everything now. */
void refresh();

/** Number of flushes so far (all displays). */
int flush_count();

}  // namespace lvgl_env

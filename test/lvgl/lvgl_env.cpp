// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0

#include "lvgl_env.h"

namespace lvgl_env {

namespace {

bool g_inited = false;
uint32_t g_tick_ms = 0;
int g_flushes = 0;

uint32_t tick_cb() { return g_tick_ms; }

void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
  (void)area;
  (void)px_map;
  ++g_flushes;
  lv_display_flush_ready(disp);
}

}  // namespace

void init() {
  if (g_inited) return;
  lv_init();
  lv_tick_set_cb(tick_cb);
  g_inited = true;
}

void deinit() {
  if (!g_inited) return;
  lv_deinit();
  g_inited = false;
}

void create_display(Display* out, int32_t width, int32_t height, lv_color_format_t format, bool full_frame) {
  out->width = width;
  out->height = height;
  out->format = format;
  out->stride = lv_draw_buf_width_to_stride(static_cast<uint32_t>(width), format);
  const uint32_t lines = full_frame ? static_cast<uint32_t>(height) : 40u;
  out->pixels.assign(static_cast<size_t>(out->stride) * lines, 0);
  out->display = lv_display_create(width, height);
  lv_display_set_color_format(out->display, format);
  lv_display_set_buffers(out->display, out->pixels.data(), nullptr, static_cast<uint32_t>(out->pixels.size()),
                         full_frame ? LV_DISPLAY_RENDER_MODE_DIRECT : LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(out->display, flush_cb);
  lv_display_set_default(out->display);
}

void run(int passes, uint32_t ms) {
  for (int i = 0; i < passes; ++i) {
    g_tick_ms += ms;
    lv_timer_handler();
  }
}

void refresh() {
  lv_refr_now(nullptr);
}

int flush_count() { return g_flushes; }

}  // namespace lvgl_env

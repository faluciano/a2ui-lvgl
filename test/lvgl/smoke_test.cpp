// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Host smoke test for the vendored LVGL build: a display backed by a memory
// buffer, a few widgets, a few timer-handler passes, and checks on the object
// tree. Also exercises the a2ui_lvgl placeholder so the a2ui_core ->
// a2ui_lvgl -> LVGL chain links on the host.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "a2ui/json.h"
#include "a2ui_lvgl/a2ui_lvgl.h"
#include "lvgl.h"

namespace {

constexpr int32_t kWidth = 240;
constexpr int32_t kHeight = 160;
constexpr uint32_t kBufLines = 20;

int g_failures = 0;
int g_flushes = 0;
uint32_t g_tick_ms = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

uint32_t tick_cb() { return g_tick_ms; }

void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
  (void)area;
  (void)px_map;
  ++g_flushes;
  lv_display_flush_ready(disp);
}

void run_timers(int passes) {
  for (int i = 0; i < passes; ++i) {
    g_tick_ms += 40;
    lv_timer_handler();
  }
}

bool label_text_is(lv_obj_t* obj, const char* expected) {
  return obj != nullptr && lv_obj_check_type(obj, &lv_label_class) &&
         std::strcmp(lv_label_get_text(obj), expected) == 0;
}

const char kMessages[] = R"json({
  "messages": [
    {"version": "1.0", "createSurface": {"surfaceId": "s", "catalogId": "basic"}},
    {"version": "1.0", "updateComponents": {"surfaceId": "s", "components": [
      {"id": "root", "component": "Column", "children": ["a", "b"]},
      {"id": "a", "component": "Text", "text": "Hello"},
      {"id": "b", "component": "Text", "text": "World"}
    ]}}
  ]
})json";

}  // namespace

int main() {
  lv_init();
  lv_tick_set_cb(tick_cb);

  std::vector<uint8_t> buf(kWidth * kBufLines * 2);  // RGB565
  lv_display_t* disp = lv_display_create(kWidth, kHeight);
  CHECK(disp != nullptr);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(disp, buf.data(), nullptr, static_cast<uint32_t>(buf.size()),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush_cb);

  lv_obj_t* screen = lv_screen_active();
  CHECK(screen != nullptr);
  CHECK(lv_obj_get_child_count(screen) == 0);

  // Plain widgets: a flex column with a label and a button holding a label.
  lv_obj_t* column = lv_obj_create(screen);
  lv_obj_set_size(column, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
  lv_obj_t* title = lv_label_create(column);
  lv_label_set_text(title, "Title");
  lv_obj_t* button = lv_button_create(column);
  lv_obj_t* button_label = lv_label_create(button);
  lv_label_set_text_fmt(button_label, "Count %d", 3);

  run_timers(5);
  CHECK(g_flushes > 0);
  CHECK(lv_obj_get_child_count(screen) == 1);
  CHECK(lv_obj_get_child_count(column) == 2);
  CHECK(lv_obj_get_child_count(button) == 1);
  CHECK(label_text_is(lv_obj_get_child(column, 0), "Title"));
  CHECK(label_text_is(lv_obj_get_child(button, 0), "Count 3"));
  CHECK(lv_obj_check_type(lv_obj_get_child(column, 1), &lv_button_class));
  // The flex layout ran: the button sits below the title.
  CHECK(lv_obj_get_y(button) > lv_obj_get_y(title));
  CHECK(lv_obj_get_height(column) > lv_obj_get_height(button));

  // The a2ui_lvgl placeholder on top of a parsed A2UI message list.
  a2ui::Json::ParseResult parsed = a2ui::Json::parse(kMessages);
  CHECK(parsed.ok);
  std::vector<std::string> texts = a2ui::lvgl::collect_component_text(parsed.value);
  CHECK(texts.size() == 2);
  lv_obj_t* placeholder = a2ui::lvgl::render_placeholder(screen, parsed.value);
  run_timers(3);
  CHECK(lv_obj_get_child_count(screen) == 2);
  CHECK(lv_obj_get_child_count(placeholder) == 2);
  CHECK(label_text_is(lv_obj_get_child(placeholder, 0), "Hello"));
  CHECK(label_text_is(lv_obj_get_child(placeholder, 1), "World"));

  // Deleting a subtree updates the tree.
  lv_obj_delete(column);
  run_timers(2);
  CHECK(lv_obj_get_child_count(screen) == 1);
  CHECK(lv_obj_get_child(screen, 0) == placeholder);

  lv_display_delete(disp);
  lv_deinit();

  if (g_failures != 0) {
    std::fprintf(stderr, "lvgl smoke test: %d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("lvgl smoke test: ok (%d flushes)\n", g_flushes);
  return 0;
}

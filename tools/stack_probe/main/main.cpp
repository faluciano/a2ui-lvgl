// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Stack probe for a2ui-lvgl: runs each scenario in a fresh FreeRTOS task and
// reports the peak stack it used (task stack size minus the high-water mark),
// the heap the scenario kept, and the free internal heap afterwards. See
// ../README.md.
//
// Scenarios share one MessageProcessor and one headless LVGL display, and run
// one after another, so each stack figure is that scenario's own peak:
//   catalog   load the basic catalog and create the processor
//   coffee    process and render the coffee order card
//   s25-*     process, then render, a 25-level tree (stress25.json)
//   s50-*     the same at the default depth limits (stress50.json)
//   input     type into the 50-level card's TextField

#include <cinttypes>
#include <cstdio>
#include <memory>
#include <string>

#include "a2ui/basic_catalog.h"
#include "a2ui/catalog.h"
#include "a2ui/json.h"
#include "a2ui/message_processor.h"
#include "a2ui/spec_documents.h"
#include "a2ui_lvgl/a2ui_lvgl.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"

extern const char kCoffee[] asm("_binary_coffee_order_json_start");
extern const char kStress50[] asm("_binary_stress50_json_start");
extern const char kStress25[] asm("_binary_stress25_json_start");

namespace {

// ESP-IDF stack sizes are in bytes. Without PSRAM a 128 KB stack does not
// fit next to the cards; 48 KB still covers the deepest scenario.
#if CONFIG_SPIRAM
constexpr uint32_t kStackBytes = 128 * 1024;
#else
constexpr uint32_t kStackBytes = 48 * 1024;
#endif
constexpr int kHres = 320;
constexpr int kVres = 480;

struct State {
  a2ui::CatalogPtr catalog;
  std::unique_ptr<a2ui::MessageProcessor> processor;
  std::unique_ptr<a2ui_lvgl::Surface> coffee;
  std::unique_ptr<a2ui_lvgl::Surface> stress50;
  std::unique_ptr<a2ui_lvgl::Surface> stress25;
  a2ui_lvgl::Theme theme = a2ui_lvgl::Theme::light();
  lv_display_t* disp = nullptr;
  bool ok = true;
};
State* g = nullptr;

void flush(lv_display_t* d, const lv_area_t*, uint8_t*) { lv_display_flush_ready(d); }
uint32_t tick_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void fail(const char* what, const std::string& why) {
  printf("PROBE-ERROR %s: %s\n", what, why.c_str());
  g->ok = false;
}

bool process(const char* name, const char* text) {
  a2ui::Json::ParseResult parsed = a2ui::Json::parse(text);
  if (!parsed.ok) {
    fail(name, "invalid JSON: " + parsed.error);
    return false;
  }
  const a2ui::Json& messages = parsed.value.is_object() ? parsed.value["messages"] : parsed.value;
  a2ui::Status st = g->processor->process_messages(messages);
  if (!st.ok()) {
    fail(name, st.error().message);
    return false;
  }
  return true;
}

std::unique_ptr<a2ui_lvgl::Surface> render(const char* surface_id) {
  a2ui::SurfaceModel* model = g->processor->model().get_surface(surface_id);
  if (model == nullptr) {
    fail(surface_id, "no such surface");
    return nullptr;
  }
  a2ui_lvgl::SurfaceConfig config;
  config.registry = &a2ui_lvgl::basic_registry();
  config.theme = &g->theme;
  lv_obj_t* screen = lv_obj_create(nullptr);
  auto surface = std::make_unique<a2ui_lvgl::Surface>(*model, screen, config);
  lv_screen_load(screen);
  lv_refr_now(g->disp);
  return surface;
}

lv_obj_t* find_textarea(lv_obj_t* obj) {
  if (lv_obj_check_type(obj, &lv_textarea_class)) return obj;
  for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
    if (lv_obj_t* hit = find_textarea(lv_obj_get_child(obj, static_cast<int32_t>(i)))) return hit;
  }
  return nullptr;
}

// --- scenarios -------------------------------------------------------------

void scenario_catalog() {
#if CONFIG_PROBE_CATALOG_SPEC_PARSED
  // As 0.1.0 documented: the parsed document stays cached next to the catalog's copy.
  auto result = a2ui::Catalog::from_json(*a2ui::spec::parsed(a2ui::spec::Document::BasicCatalogV1));
  if (result.ok()) a2ui::basic_catalog::bind_functions(*result.value());
#else
  // Compact when CONFIG_A2UI_COMPACT_CATALOG is set.
  auto result = a2ui::basic_catalog::load();
#endif
  if (!result.ok()) return fail("catalog", result.error().message);
  g->catalog = result.value();
  a2ui::MessageProcessorOptions options;
  options.outbound_listener = [](const a2ui::Json& m) {
    printf("PROBE-OUTBOUND %s\n", m.dump().substr(0, 200).c_str());
  };
  g->processor = std::make_unique<a2ui::MessageProcessor>(std::vector<a2ui::CatalogPtr>{g->catalog}, options);
}

void scenario_coffee() {
  if (process("coffee", kCoffee)) g->coffee = render("gallery-coffee-order");
}

void scenario_stress50_process() { process("stress50", kStress50); }
void scenario_stress50_render() { g->stress50 = render("stress50"); }
void scenario_stress25_process() { process("stress25", kStress25); }
void scenario_stress25_render() { g->stress25 = render("stress25"); }

void scenario_input() {
  lv_obj_t* ta = find_textarea(lv_screen_active());
  if (ta == nullptr) return fail("input", "no TextField on screen");
  // Each change writes /form/name, re-runs the 5-deep check and re-resolves
  // the 50 Text labels that read it; the last value fails the check.
  for (const char* text : {"abcdef", "abcdefgh", "abc123"}) {
    lv_textarea_set_text(ta, text);
    lv_timer_handler();
  }
}

struct Job {
  const char* name;
  void (*fn)();
  SemaphoreHandle_t done;
};

void run_job(void* arg) {
  auto* job = static_cast<Job*>(arg);
  const size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  job->fn();
  const long kept = static_cast<long>(heap_before) - static_cast<long>(heap_caps_get_free_size(MALLOC_CAP_8BIT));
  const UBaseType_t free_bytes = uxTaskGetStackHighWaterMark(nullptr);
  printf("PROBE-STACK %-8s used %6" PRIu32 " of %" PRIu32 " bytes; heap kept %+7ld; internal heap free %u, min %u\n",
         job->name, kStackBytes - static_cast<uint32_t>(free_bytes), kStackBytes, kept,
         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
         static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
  xSemaphoreGive(job->done);
  vTaskDelete(nullptr);
}

void run(const char* name, void (*fn)()) {
  Job job{name, fn, xSemaphoreCreateBinary()};
  if (xTaskCreate(run_job, name, kStackBytes, &job, 5, nullptr) != pdPASS) {
    printf("PROBE-ERROR %s: task create failed\n", name);
    g->ok = false;
    return;
  }
  xSemaphoreTake(job.done, portMAX_DELAY);
  vSemaphoreDelete(job.done);
  // Let the idle task free the finished task's stack, so the next scenario's
  // "heap kept" does not include it.
  vTaskDelay(pdMS_TO_TICKS(20));
}

}  // namespace

extern "C" void app_main(void) {
  g = new State();
  lv_init();
  lv_tick_set_cb(tick_ms);
  const uint32_t buf_bytes = kHres * 20 * sizeof(uint16_t);
  void* buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_8BIT);
  g->disp = lv_display_create(kHres, kVres);
  lv_display_set_color_format(g->disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(g->disp, buf, nullptr, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(g->disp, flush);
  a2ui_lvgl::apply_lvgl_theme(g->disp, g->theme);

#if CONFIG_PROBE_CATALOG_SPEC_PARSED
  const char* catalog_kind = "spec::parsed + from_json";
#else
  const char* catalog_kind = a2ui::basic_catalog::compact_by_default() ? "compact" : "Json";
#endif
  printf("PROBE-START internal heap free %u, %s catalog\n",
         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)), catalog_kind);
  run("catalog", scenario_catalog);
  if (g->ok) run("coffee", scenario_coffee);
#if !CONFIG_SPIRAM
  // Without PSRAM the stress cards do not fit next to the coffee card (an
  // ESP32-C3 runs out of heap processing stress25), and running out of
  // memory aborts.
  printf("PROBE-SKIP stress scenarios: no PSRAM\n");
  printf("PROBE-DONE %s\n", g->ok ? "ok" : "failed");
  return;
#endif
  if (g->ok) run("s25-proc", scenario_stress25_process);
  if (g->ok) run("s25-rend", scenario_stress25_render);
  if (g->ok) run("s50-proc", scenario_stress50_process);
  if (g->ok) run("s50-rend", scenario_stress50_render);
  if (g->ok) run("input", scenario_input);
  printf("PROBE-DONE %s\n", g->ok ? "ok" : "failed");
}

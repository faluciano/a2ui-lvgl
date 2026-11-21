// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// A2UI basic surface example: feeds an A2UI v1.0 sample (coffee_order.json,
// a copy of the upstream catalogs/basic/v1/examples/13_coffee-order.json,
// embedded at build time) through the a2ui_core MessageProcessor and renders
// it with the a2ui_lvgl adapter. Button actions are logged (they would go to
// the agent).
//
// The display is chosen in menuconfig ("A2UI basic surface example"):
// an SPI ST7789/ILI9341 panel through esp_lcd, or a headless display whose
// flush callback discards the pixels so the example runs on any board.
//
// Everything A2UI runs on the LVGL task's lock: the core and the adapter
// are single-threaded, like LVGL.

#include <cinttypes>
#include <memory>

#include "a2ui/basic_catalog.h"
#include "a2ui/catalog.h"
#include "a2ui/json.h"
#include "a2ui/message_processor.h"
#include "a2ui/spec_documents.h"
#include "a2ui_lvgl/a2ui_lvgl.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"

#if CONFIG_A2UI_EXAMPLE_DISPLAY_SPI
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#if CONFIG_A2UI_EXAMPLE_LCD_ILI9341
#include "esp_lcd_ili9341.h"
#endif
#endif

// The embedded sample (EMBED_TXTFILES adds a terminating NUL).
extern const char kSampleStart[] asm("_binary_coffee_order_json_start");

namespace {

constexpr const char* kTag = "a2ui_example";
constexpr uint32_t kHres = CONFIG_A2UI_EXAMPLE_HRES;
constexpr uint32_t kVres = CONFIG_A2UI_EXAMPLE_VRES;
constexpr uint32_t kBufLines = 40;

#if CONFIG_A2UI_EXAMPLE_DISPLAY_SPI

lv_display_t* add_spi_display() {
  spi_bus_config_t bus = {};
  bus.sclk_io_num = static_cast<gpio_num_t>(CONFIG_A2UI_EXAMPLE_PIN_SCLK);
  bus.mosi_io_num = static_cast<gpio_num_t>(CONFIG_A2UI_EXAMPLE_PIN_MOSI);
  bus.miso_io_num = GPIO_NUM_NC;
  bus.quadwp_io_num = GPIO_NUM_NC;
  bus.quadhd_io_num = GPIO_NUM_NC;
  bus.max_transfer_sz = static_cast<int>(kHres * kBufLines * sizeof(uint16_t));
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

  esp_lcd_panel_io_handle_t io = nullptr;
  esp_lcd_panel_io_spi_config_t io_cfg = {};
  io_cfg.cs_gpio_num = static_cast<gpio_num_t>(CONFIG_A2UI_EXAMPLE_PIN_CS);
  io_cfg.dc_gpio_num = static_cast<gpio_num_t>(CONFIG_A2UI_EXAMPLE_PIN_DC);
  io_cfg.spi_mode = 0;
  io_cfg.pclk_hz = CONFIG_A2UI_EXAMPLE_LCD_PIXEL_CLOCK_MHZ * 1000 * 1000;
  io_cfg.trans_queue_depth = 10;
  io_cfg.lcd_cmd_bits = 8;
  io_cfg.lcd_param_bits = 8;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &io));

  esp_lcd_panel_handle_t panel = nullptr;
  esp_lcd_panel_dev_config_t panel_cfg = {};
  panel_cfg.reset_gpio_num = static_cast<gpio_num_t>(CONFIG_A2UI_EXAMPLE_PIN_RST);
#if CONFIG_A2UI_EXAMPLE_LCD_BGR
  panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
#else
  panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
#endif
  panel_cfg.bits_per_pixel = 16;
#if CONFIG_A2UI_EXAMPLE_LCD_ILI9341
  ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &panel_cfg, &panel));
#else
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));
#endif
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
#if CONFIG_A2UI_EXAMPLE_LCD_INVERT_COLORS
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
#endif
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

  if (CONFIG_A2UI_EXAMPLE_PIN_BL >= 0) {
    gpio_config_t bl = {};
    bl.pin_bit_mask = 1ULL << CONFIG_A2UI_EXAMPLE_PIN_BL;
    bl.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&bl));
    gpio_set_level(static_cast<gpio_num_t>(CONFIG_A2UI_EXAMPLE_PIN_BL),
                   CONFIG_A2UI_EXAMPLE_BL_ON_LEVEL);
  }

  lvgl_port_display_cfg_t disp_cfg = {};
  disp_cfg.io_handle = io;
  disp_cfg.panel_handle = panel;
  disp_cfg.buffer_size = kHres * kBufLines;
  disp_cfg.double_buffer = true;
  disp_cfg.hres = kHres;
  disp_cfg.vres = kVres;
  disp_cfg.color_format = LV_COLOR_FORMAT_RGB565;
  disp_cfg.flags.buff_dma = 1;
  disp_cfg.flags.swap_bytes = 1;  // SPI panels expect big-endian RGB565.
  return lvgl_port_add_disp(&disp_cfg);
}

#else  // CONFIG_A2UI_EXAMPLE_DISPLAY_HEADLESS

void headless_flush(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
  (void)area;
  (void)px_map;
  lv_display_flush_ready(disp);
}

lv_display_t* add_headless_display() {
  const uint32_t buf_bytes = kHres * kBufLines * sizeof(uint16_t);
  void* buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_8BIT);
  if (buf == nullptr) return nullptr;
  if (!lvgl_port_lock(0)) return nullptr;
  lv_display_t* disp = lv_display_create(kHres, kVres);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(disp, buf, nullptr, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, headless_flush);
  lvgl_port_unlock();
  return disp;
}

#endif

void log_heap(const char* when) {
  ESP_LOGI(kTag, "%s: free heap %u bytes (internal %u, min ever %u)", when,
           static_cast<unsigned>(esp_get_free_heap_size()),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(esp_get_minimum_free_heap_size()));
}

/** The A2UI side: processor, the rendered surface, and the action log. Lives forever. */
struct App {
  std::unique_ptr<a2ui::MessageProcessor> processor;
  std::unique_ptr<a2ui_lvgl::Surface> surface;
  a2ui::Subscription actions;
  a2ui_lvgl::Theme theme = a2ui_lvgl::Theme::light();
};

App* start_a2ui(lv_display_t* disp) {
  auto result = a2ui::Catalog::from_json(*a2ui::spec::parsed(a2ui::spec::Document::BasicCatalogV1));
  if (!result.ok()) {
    ESP_LOGE(kTag, "basic catalog: %s", result.error().message.c_str());
    return nullptr;
  }
  a2ui::CatalogPtr catalog = result.value();
  a2ui::basic_catalog::bind_functions(*catalog);

  auto* app = new App();
  a2ui::MessageProcessorOptions options;
  options.outbound_listener = [](const a2ui::Json& message) {
    ESP_LOGW(kTag, "to agent: %s", message.dump().c_str());
  };
  app->processor = std::make_unique<a2ui::MessageProcessor>(std::vector<a2ui::CatalogPtr>{catalog}, options);
  // Actions would be sent to the agent; here they are logged.
  app->actions = app->processor->model().on_action().subscribe([](const a2ui::Json& action) {
    ESP_LOGI(kTag, "action %s from %s: %s", action["name"].as_string().c_str(),
             action["sourceComponentId"].as_string().c_str(), action["context"].dump().c_str());
  });

  a2ui::Json::ParseResult parsed = a2ui::Json::parse(kSampleStart);
  if (!parsed.ok) {
    ESP_LOGE(kTag, "sample JSON invalid at %u: %s", static_cast<unsigned>(parsed.offset), parsed.error.c_str());
    return app;
  }
  a2ui::Status st = app->processor->process_messages(parsed.value["messages"]);
  if (!st.ok()) ESP_LOGW(kTag, "sample: %s", st.error().message.c_str());

  a2ui_lvgl::apply_lvgl_theme(disp, app->theme);
  for (const auto& entry : app->processor->model().surfaces()) {
    a2ui_lvgl::SurfaceConfig config;
    config.registry = &a2ui_lvgl::basic_registry();
    config.theme = &app->theme;
    app->surface = std::make_unique<a2ui_lvgl::Surface>(*entry.second, lv_display_get_screen_active(disp), config);
    ESP_LOGI(kTag, "rendering surface '%s'", entry.first.c_str());
    break;
  }
  return app;
}

uint32_t count_views(a2ui_lvgl::NodeView* view) {
  if (view == nullptr) return 0;
  uint32_t n = 1;
  for (a2ui_lvgl::NodeView* child : view->children()) n += count_views(child);
  return n;
}

}  // namespace

extern "C" void app_main(void) {
  lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
  // Event handlers run A2UI actions and bindings on the LVGL task.
  port_cfg.task_stack = 16384;
  ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

#if CONFIG_A2UI_EXAMPLE_DISPLAY_SPI
  lv_display_t* disp = add_spi_display();
#else
  lv_display_t* disp = add_headless_display();
#endif
  if (disp == nullptr) {
    ESP_LOGE(kTag, "display setup failed");
    return;
  }
  log_heap("before A2UI");

  if (!lvgl_port_lock(0)) return;
  App* app = start_a2ui(disp);
  const uint32_t views = app && app->surface ? count_views(app->surface->root_view()) : 0;
  lvgl_port_unlock();

  ESP_LOGI(kTag, "rendered %" PRIu32 " component view(s)", views);
  log_heap("after render");
}

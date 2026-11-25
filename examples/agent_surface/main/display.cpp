// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Display setup, copied from examples/basic_surface/main/main.cpp.

#include "display.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
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

namespace {

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

}  // namespace

lv_display_t* example_add_display() {
#if CONFIG_A2UI_EXAMPLE_DISPLAY_SPI
  return add_spi_display();
#else
  return add_headless_display();
#endif
}

// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// A2UI agent surface example: joins Wi-Fi, connects to an A2UI v1.0 agent
// over a WebSocket (components/a2ui_transport_ws) and draws the surface the
// agent sends. The user's actions go back to the agent, which answers with
// updates. sample_agent.py in this directory is an agent to try it with.
//
// Wi-Fi, the agent's address and the display are set in menuconfig
// ("A2UI agent surface example").
//
// Threads: the WebSocket client has its own task, which only queues what it
// receives. Everything A2UI (the processor, the surface views, sending to
// the agent) runs on the LVGL task, in the lv_timer callback below.

#include <cinttypes>
#include <memory>
#include <string>

#include "a2ui/basic_catalog.h"
#include "a2ui/catalog.h"
#include "a2ui/message_processor.h"
#include "a2ui/spec_documents.h"
#include "a2ui_lvgl/a2ui_lvgl.h"
#include "a2ui_transport_ws/ws_transport.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include "wifi.h"

namespace {

constexpr const char* kTag = "a2ui_agent";
constexpr uint32_t kPollPeriodMs = 20;
constexpr uint32_t kWifiWaitMs = 30000;

struct App {
  lv_display_t* disp = nullptr;
  a2ui_lvgl::Theme theme = a2ui_lvgl::Theme::light();
  std::unique_ptr<a2ui::MessageProcessor> processor;
  std::unique_ptr<a2ui_transport_ws::WsTransport> transport;
  std::unique_ptr<a2ui_lvgl::Surface> view;
  lv_obj_t* status = nullptr;  // A line of text on the top layer.
  bool ever_connected = false;
};

void log_heap(const char* when) {
  ESP_LOGI(kTag, "%s: free heap %u bytes (internal %u, min ever %u)", when,
           static_cast<unsigned>(esp_get_free_heap_size()),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(esp_get_minimum_free_heap_size()));
}

void set_status(App& app, const char* text) {
  if (text == nullptr) {
    lv_obj_set_hidden(app.status, true);
    return;
  }
  if (std::string(lv_label_get_text(app.status)) != text) lv_label_set_text(app.status, text);
  lv_obj_set_hidden(app.status, false);
}

// Shows the first surface the agent created. The agent may delete it or the
// session may restart; the view then unmounts itself and is replaced here.
void sync_view(App& app) {
  if (app.view && !app.view->mounted()) app.view.reset();
  if (app.view) return;
  for (const auto& entry : app.processor->model().surfaces()) {
    a2ui_lvgl::SurfaceConfig config;
    config.registry = &a2ui_lvgl::basic_registry();
    config.theme = &app.theme;
    app.view = std::make_unique<a2ui_lvgl::Surface>(*entry.second, lv_display_get_screen_active(app.disp), config);
    ESP_LOGI(kTag, "showing surface '%s'", entry.first.c_str());
    log_heap("surface shown");
    break;
  }
}

void sync_status(App& app) {
  const bool connected = app.transport->connected();
  app.ever_connected = app.ever_connected || connected;
  if (connected) {
    set_status(app, app.view ? nullptr : "Connected. Waiting for the agent...");
  } else if (app.ever_connected) {
    set_status(app, "Agent offline. Reconnecting...");
  } else {
    set_status(app, "Connecting to " CONFIG_A2UI_EXAMPLE_AGENT_URI);
  }
}

// Runs on the LVGL task, with the LVGL lock held.
void on_poll(lv_timer_t* timer) {
  App& app = *static_cast<App*>(lv_timer_get_user_data(timer));
  app.transport->poll();
  app.processor->tick(static_cast<uint64_t>(esp_timer_get_time() / 1000));
  sync_view(app);
  sync_status(app);
}

App* create_app(lv_display_t* disp) {
  auto catalog = a2ui::Catalog::from_json(*a2ui::spec::parsed(a2ui::spec::Document::BasicCatalogV1));
  if (!catalog.ok()) {
    ESP_LOGE(kTag, "basic catalog: %s", catalog.error().message.c_str());
    return nullptr;
  }
  a2ui::basic_catalog::bind_functions(*catalog.value());

  auto* app = new App();
  app->disp = disp;
  a2ui::MessageProcessorOptions options;
  options.forward_actions = true;  // The session sends the user's actions to the agent.
  app->processor = std::make_unique<a2ui::MessageProcessor>(std::vector<a2ui::CatalogPtr>{catalog.value()},
                                                            std::move(options));

  a2ui_transport_ws::WsTransportConfig config;
  config.uri = CONFIG_A2UI_EXAMPLE_AGENT_URI;
  config.reconnect_ms = CONFIG_A2UI_EXAMPLE_RECONNECT_MS;
  config.max_message_size = static_cast<size_t>(CONFIG_A2UI_EXAMPLE_MAX_MESSAGE_KB) * 1024;
  app->transport = std::make_unique<a2ui_transport_ws::WsTransport>(*app->processor, std::move(config));

  a2ui_lvgl::apply_lvgl_theme(disp, app->theme);
  lv_obj_set_style_bg_color(lv_display_get_screen_active(disp), app->theme.background, 0);
  app->status = lv_label_create(lv_display_get_layer_top(disp));
  lv_label_set_long_mode(app->status, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_width(app->status, lv_pct(100));
  lv_obj_set_style_text_align(app->status, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_bg_opa(app->status, LV_OPA_80, 0);
  lv_obj_set_style_bg_color(app->status, app->theme.background, 0);
  lv_obj_set_style_pad_all(app->status, 6, 0);
  lv_obj_align(app->status, LV_ALIGN_BOTTOM_MID, 0, 0);
  set_status(*app, "Joining Wi-Fi " CONFIG_A2UI_EXAMPLE_WIFI_SSID);
  return app;
}

}  // namespace

extern "C" void app_main(void) {
  lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
  // Everything A2UI runs on the LVGL task. An agent can send messages at the
  // default depth limits, which need about 40 KB of stack (docs/porting.md,
  // "Stack").
  port_cfg.task_stack = 40 * 1024;
  ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

  lv_display_t* disp = example_add_display();
  if (disp == nullptr) {
    ESP_LOGE(kTag, "display setup failed");
    return;
  }

  if (!lvgl_port_lock(0)) return;
  App* app = create_app(disp);
  lvgl_port_unlock();
  if (app == nullptr) return;
  log_heap("catalog loaded");

  if (!example_wifi_start(kWifiWaitMs)) {
    ESP_LOGW(kTag, "no IP address after %" PRIu32 " s; connecting to the agent anyway", kWifiWaitMs / 1000);
  }

  if (!lvgl_port_lock(0)) return;
  const esp_err_t err = app->transport->start();
  if (err == ESP_OK) {
    lv_timer_create(on_poll, kPollPeriodMs, app);
    sync_status(*app);
  } else {
    ESP_LOGE(kTag, "WebSocket client: %s", esp_err_to_name(err));
    set_status(*app, "WebSocket client failed to start");
  }
  lvgl_port_unlock();
}

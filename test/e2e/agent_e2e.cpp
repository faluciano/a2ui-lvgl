// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// End-to-end test of the WebSocket transport with the sample agent
// (examples/agent_surface/sample_agent.py), on the host:
//
//   agent_e2e ws://127.0.0.1:PORT
//
// Connects with the same AgentSession the device uses, renders what the agent
// sends with a2ui_lvgl on a headless display, presses buttons and checks that
// the agent's updates arrive and are drawn. Then drops the connection and
// checks that the next session restores the data model. test/e2e/run.sh
// starts the agent and runs this (make e2e).

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "a2ui/basic_catalog.h"
#include "a2ui/catalog.h"
#include "a2ui/message_processor.h"
#include "a2ui/spec_documents.h"
#include "a2ui_lvgl/a2ui_lvgl.h"
#include "a2ui_transport_ws/agent_session.h"
#include "lvgl_env.h"
#include "ws_client.h"

namespace {

constexpr const char* kSurface = "gallery-coffee-order";
constexpr const char* kBasicCatalog = "https://a2ui.org/specification/v1_0/catalogs/basic/catalog.json";
constexpr int kTimeoutMs = 10000;

int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                    \
    }                                                                  \
  } while (0)

#define REQUIRE(cond)                                                  \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                        \
    }                                                                  \
  } while (0)

a2ui::Json parse(const std::string& text) {
  a2ui::Json::ParseResult r = a2ui::Json::parse(text);
  return r.ok ? r.value : a2ui::Json();
}

struct Harness {
  a2ui::CatalogPtr catalog;
  std::unique_ptr<a2ui::MessageProcessor> processor;
  std::unique_ptr<a2ui_transport_ws::AgentSession> session;
  std::unique_ptr<a2ui_lvgl::Surface> view;
  e2e::WsClient ws;
  std::vector<a2ui::Json> sent;  // Every frame the session sent, parsed.
  int errors = 0;                // Error envelopes the processor produced.

  // What the example app does on each timer tick.
  void sync_view() {
    if (view && !view->mounted()) view.reset();
    if (view) return;
    for (const auto& entry : processor->model().surfaces()) {
      a2ui_lvgl::SurfaceConfig config;
      config.registry = &a2ui_lvgl::basic_registry();
      view = std::make_unique<a2ui_lvgl::Surface>(*entry.second, lv_screen_active(), config);
      break;
    }
  }

  bool connect(const std::string& url) {
    std::string error;
    if (!ws.connect(url, &error)) {
      std::fprintf(stderr, "connect %s: %s\n", url.c_str(), error.c_str());
      return false;
    }
    session->connected();
    return true;
  }

  void disconnect() {
    ws.close();
    session->disconnected();
  }

  /** Runs the loop until `done` holds or the timeout passes. */
  bool pump_until(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
      std::vector<std::string> frames;
      if (!ws.poll(10, &frames) && session->is_connected()) session->disconnected();
      for (const std::string& f : frames) {
        a2ui::Status st = session->receive(f);
        if (!st.ok()) std::fprintf(stderr, "rejected: %s\n", st.error().message.c_str());
      }
      sync_view();
      lvgl_env::run(1, 10);
      if (done()) return true;
    }
    std::fprintf(stderr, "timed out: %zu item rows, total \"%s\", items %s\n", item_rows(),
                 text_of("total_value").c_str(), data("/items").dump().c_str());
    return false;
  }

  a2ui::Json data(const char* pointer) const {
    a2ui::SurfaceModel* s = processor->model().get_surface(kSurface);
    return s ? s->data_model().value_at(pointer) : a2ui::Json();
  }

  a2ui_lvgl::NodeView* find(const char* id) const { return view ? view->find_view(id) : nullptr; }

  std::string text_of(const char* id) const {
    a2ui_lvgl::NodeView* v = find(id);
    return v ? v->describe()["text"].as_string() : std::string();
  }

  size_t item_rows() const {
    a2ui_lvgl::NodeView* list = find("items_list");
    return list ? list->children().size() : 0;
  }
};

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: agent_e2e ws://host:port\n");
    return 2;
  }
  const std::string url = argv[1];

  lvgl_env::init();
  lvgl_env::Display disp;
  lvgl_env::create_display(&disp, 320, 480, LV_COLOR_FORMAT_RGB565, false);

  Harness h;
  {
    auto cat = a2ui::Catalog::from_json(*a2ui::spec::parsed(a2ui::spec::Document::BasicCatalogV1));
    REQUIRE(cat.ok());
    h.catalog = cat.value();
    a2ui::basic_catalog::bind_functions(*h.catalog);
    a2ui::MessageProcessorOptions options;
    options.forward_actions = true;
    h.processor = std::make_unique<a2ui::MessageProcessor>(std::vector<a2ui::CatalogPtr>{h.catalog}, options);
  }
  a2ui::Subscription errors = h.processor->on_error().subscribe([&h](const a2ui::Json& e) {
    std::fprintf(stderr, "processor error: %s\n", e.dump().c_str());
    ++h.errors;
  });
  h.session = std::make_unique<a2ui_transport_ws::AgentSession>(*h.processor, [&h](const std::string& frame) {
    h.sent.push_back(parse(frame));
    return h.ws.send_text(frame);
  });

  // 1. The first session: capabilities go out, the surface comes back.
  REQUIRE(h.connect(url));
  REQUIRE(h.sent.size() == 1);
  {
    const a2ui::Json& meta = h.sent[0]["metadata"];
    const a2ui::Json& ids = meta["a2uiRendererCapabilities"]["1.0"]["supportedCatalogIds"];
    CHECK(ids.size() == 1 && ids[0].as_string() == kBasicCatalog);
    CHECK(!meta.contains("a2uiRendererDataModel"));
    CHECK(!h.sent[0].contains("message"));
  }
  REQUIRE(h.pump_until([&] { return h.find("add_btn") != nullptr && h.item_rows() == 2; }));
  CHECK(h.data("/storeName").as_string() == "Sunrise Coffee");
  CHECK(h.text_of("total_value") == "$11.66");
  std::printf("ok: surface rendered (%zu item rows, total %s)\n", h.item_rows(), h.text_of("total_value").c_str());

  // 2. An action goes out with the data model; the agent's update is drawn.
  REQUIRE(h.find("add_btn")->simulate_gesture());
  {
    const a2ui::Json& last = h.sent.back();
    CHECK(last["message"]["action"]["name"].as_string() == "add_to_cart");
    CHECK(last["message"]["action"]["surfaceId"].as_string() == kSurface);
    CHECK(last["metadata"]["a2uiRendererDataModel"]["surfaces"][kSurface]["items"].size() == 2);
  }
  REQUIRE(h.pump_until([&] { return h.item_rows() == 3 && h.text_of("total_value") == "$17.06"; }));
  CHECK(h.data("/items")[2]["name"].as_string() == "Cold Brew");
  std::printf("ok: add_to_cart answered (%zu item rows, total %s)\n", h.item_rows(), h.text_of("total_value").c_str());

  // 3. The connection drops: the surface stays on screen, actions are dropped.
  h.disconnect();
  CHECK(h.view && h.view->mounted());
  const uint32_t dropped = h.session->stats().dropped_out;
  REQUIRE(h.find("add_btn")->simulate_gesture());
  CHECK(h.session->stats().dropped_out == dropped + 1);

  // 4. A new session deletes the old surface and offers its data model; the
  //    agent creates the surface again with that data.
  const size_t before = h.sent.size();
  REQUIRE(h.connect(url));
  CHECK(h.processor->model().get_surface(kSurface) == nullptr);
  REQUIRE(h.sent.size() == before + 1);
  CHECK(h.sent.back()["metadata"]["a2uiRendererDataModel"]["surfaces"][kSurface]["items"].size() == 3);
  REQUIRE(h.pump_until([&] { return h.item_rows() == 3 && h.text_of("total_value") == "$17.06"; }));
  std::printf("ok: reconnect restored the data model (%zu item rows)\n", h.item_rows());

  // 5. One more action in the new session.
  REQUIRE(h.find("purchase_btn")->simulate_gesture());
  REQUIRE(h.pump_until([&] { return h.item_rows() == 0; }));
  CHECK(h.data("/storeName").as_string() == "Ordered 3 items. Thank you!");
  CHECK(h.text_of("total_value") == "$0.00");
  std::printf("ok: purchase answered (\"%s\")\n", h.data("/storeName").as_string().c_str());

  CHECK(h.errors == 0);
  CHECK(h.session->stats().frames_rejected == 0);
  CHECK(h.session->stats().sessions == 2);

  h.disconnect();
  h.view.reset();
  h.session.reset();
  errors.unsubscribe();
  h.processor->dispose();
  lv_display_delete(disp.display);
  lvgl_env::deinit();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("agent_e2e: all checks passed\n");
  return 0;
}

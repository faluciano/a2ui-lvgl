// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Host tests for the vendored LVGL build and the a2ui_lvgl adapter, on a
// display backed by a memory buffer: plain LVGL widgets, then the adapter's
// lifecycle and edge cases that the conformance suites do not reach
// (object deletion from outside, deletion inside an event handler, real
// pointer input, fallbacks, registry overrides, Markdown, ISO 8601, text
// wrapping in rows, svgPath icons, DateTimeInput ranges).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "a2ui/basic_catalog.h"
#include "a2ui/catalog.h"
#include "a2ui/json.h"
#include "a2ui/message_processor.h"
#include "a2ui/spec_documents.h"
#include "a2ui_lvgl/a2ui_lvgl.h"
#include "catalog/basic/iso8601.h"
#include "catalog/basic/svg_path.h"
#include "lvgl.h"
#include "lvgl_env.h"

namespace {

using a2ui::Json;

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    ++g_checks;                                                                     \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

bool label_text_is(lv_obj_t* obj, const char* expected) {
  return obj != nullptr && lv_obj_check_type(obj, &lv_label_class) &&
         std::strcmp(lv_label_get_text(obj), expected) == 0;
}

Json parse(const char* text) {
  Json::ParseResult r = Json::parse(text);
  if (!r.ok) std::fprintf(stderr, "bad test JSON: %s\n", r.error.c_str());
  return r.value;
}

a2ui::CatalogPtr basic_catalog() {
  static a2ui::CatalogPtr c = [] {
    auto r = a2ui::Catalog::from_json(*a2ui::spec::parsed(a2ui::spec::Document::BasicCatalogV1));
    a2ui::basic_catalog::bind_functions(*r.value());
    return r.value();
  }();
  return c;
}

/** A processor, one surface "s" and its view on the active screen. */
struct Fixture {
  explicit Fixture(a2ui_lvgl::SurfaceConfig cfg = {}) : config(std::move(cfg)) {
    if (!config.registry) config.registry = &a2ui_lvgl::basic_registry();
    a2ui::MessageProcessorOptions options;
    options.validation_config = a2ui::ValidationConfig::relaxed();
    processor = std::make_unique<a2ui::MessageProcessor>(std::vector<a2ui::CatalogPtr>{basic_catalog()}, options);
    action_sub = processor->model().on_action().subscribe([this](const Json& a) { actions.push_back(a); });
  }
  ~Fixture() {
    view.reset();
    processor.reset();
    lvgl_env::run(2);
  }
  a2ui::Status send(const char* json) {
    a2ui::Status st = processor->process_messages(parse(json));
    if (!view) {
      if (a2ui::SurfaceModel* m = processor->model().get_surface("s")) {
        view = std::make_unique<a2ui_lvgl::Surface>(*m, lv_screen_active(), config);
      }
    }
    lvgl_env::run(2);
    lvgl_env::refresh();
    return st;
  }
  a2ui_lvgl::NodeView* find(const char* id) { return view ? view->find_view(id) : nullptr; }
  lv_obj_t* obj(const char* id) {
    a2ui_lvgl::NodeView* v = find(id);
    return v ? v->obj() : nullptr;
  }

  a2ui_lvgl::SurfaceConfig config;
  std::unique_ptr<a2ui::MessageProcessor> processor;
  std::unique_ptr<a2ui_lvgl::Surface> view;
  std::vector<Json> actions;
  a2ui::Subscription action_sub;
};

constexpr const char* kCreate =
    R"({"version":"1.0","createSurface":{"surfaceId":"s","catalogId":"https://a2ui.org/specification/v1_0/catalogs/basic/catalog.json"}})";

std::string update(const std::string& components) {
  return R"({"version":"1.0","updateComponents":{"surfaceId":"s","components":)" + components + "}}";
}

// --- Plain LVGL ------------------------------------------------------------------------

void test_plain_lvgl() {
  lv_obj_t* screen = lv_screen_active();
  lv_obj_t* column = lv_obj_create(screen);
  lv_obj_set_size(column, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
  lv_obj_t* title = lv_label_create(column);
  lv_label_set_text(title, "Title");
  lv_obj_t* button = lv_button_create(column);
  lv_obj_t* button_label = lv_label_create(button);
  lv_label_set_text_fmt(button_label, "Count %d", 3);
  lvgl_env::run(5);
  CHECK(lvgl_env::flush_count() > 0);
  CHECK(lv_obj_get_child_count(column) == 2);
  CHECK(label_text_is(lv_obj_get_child(column, 0), "Title"));
  CHECK(lv_obj_get_y(button) > lv_obj_get_y(title));
  lv_obj_delete(column);
  CHECK(lv_obj_get_child_count(screen) == 0);
}

// --- Adapter ---------------------------------------------------------------------------

void test_render_and_reorder() {
  Fixture f;
  f.send(kCreate);
  f.send(update(R"([{"id":"root","component":"Column","children":["a","b","c"]},
    {"id":"a","component":"Text","text":"A"},{"id":"b","component":"Text","text":"B"},
    {"id":"c","component":"Text","text":"C"}])").c_str());
  lv_obj_t* col = f.obj("root");
  CHECK(col != nullptr);
  CHECK(lv_obj_get_child_count(col) == 3);
  lv_obj_t* a = f.obj("a");
  lv_obj_t* c = f.obj("c");
  CHECK(label_text_is(lv_obj_get_child(col, 0), "A"));
  CHECK(lv_obj_get_y(f.obj("b")) > lv_obj_get_y(a));
  // Leaf margin on Text, none on the Column.
  CHECK(lv_obj_get_style_margin_left(a, LV_PART_MAIN) > 0);
  CHECK(lv_obj_get_style_margin_left(col, LV_PART_MAIN) == 0);
  CHECK(lv_obj_get_style_pad_left(col, LV_PART_MAIN) == 0);

  // Reorder: same objects, new order.
  f.send(update(R"([{"id":"root","component":"Column","children":["c","a","b"]}])").c_str());
  CHECK(f.obj("a") == a);
  CHECK(f.obj("c") == c);
  CHECK(lv_obj_get_child(col, 0) == c);
  CHECK(lv_obj_get_child(col, 1) == a);
  CHECK(lv_obj_get_y(a) > lv_obj_get_y(c));
  // Data-bound update reaches the label without remounting.
  f.send(update(R"([{"id":"a","component":"Text","text":"A2"}])").c_str());
  CHECK(f.obj("a") == a && label_text_is(a, "A2"));
}

void test_pending_takes_no_slot() {
  Fixture f;
  f.send(kCreate);
  f.send(update(R"([{"id":"root","component":"Row","children":["x","later","y"]},
    {"id":"x","component":"Text","text":"X"},{"id":"y","component":"Text","text":"Y"}])").c_str());
  lv_obj_t* row = f.obj("root");
  CHECK(lv_obj_get_child_count(row) == 2);
  CHECK(f.find("later") != nullptr && f.obj("later") == nullptr);
  f.send(update(R"([{"id":"later","component":"Text","text":"L"}])").c_str());
  CHECK(f.obj("root") == row);
  CHECK(lv_obj_get_child_count(row) == 3);
  CHECK(label_text_is(lv_obj_get_child(row, 1), "L"));
}

void test_fallbacks() {
  a2ui_lvgl::SurfaceConfig cfg;
  std::vector<std::string> seen;
  cfg.fallbacks.pending = [&seen](lv_obj_t* parent, const a2ui_lvgl::PendingInfo& info) -> lv_obj_t* {
    seen.push_back("pending:" + info.component_id + "<" + info.parent_component_type);
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, "...");
    return l;
  };
  cfg.fallbacks.error = [&seen](lv_obj_t*, const a2ui_lvgl::ErrorInfo& info) -> lv_obj_t* {
    seen.push_back("error:" + info.type + ":" + info.component_type);
    return nullptr;  // Nothing.
  };
  Fixture f(std::move(cfg));
  f.send(kCreate);
  CHECK(!seen.empty() && seen[0] == "pending:root<");  // Root before it arrives.
  f.send(update(R"([{"id":"root","component":"Column","children":["p","u"]},
    {"id":"u","component":"Mystery"}])").c_str());
  bool pending_child = false, unknown = false;
  for (const auto& s : seen) {
    pending_child = pending_child || s == "pending:p<Column";
    unknown = unknown || s == "error:unknown-type:Mystery";
  }
  CHECK(pending_child);
  CHECK(unknown);
  lv_obj_t* col = f.obj("root");
  CHECK(lv_obj_get_child_count(col) == 1);  // The pending fallback; the error fallback rendered nothing.
  CHECK(f.find("p")->is_fallback());
  f.send(update(R"([{"id":"p","component":"Text","text":"P"}])").c_str());
  CHECK(!f.find("p")->is_fallback());
  CHECK(label_text_is(lv_obj_get_child(col, 0), "P"));
}

void test_external_deletion() {
  Fixture f;
  f.send(kCreate);
  f.send(R"({"version":"1.0","updateDataModel":{"surfaceId":"s","path":"/t","value":"one"}})");
  f.send(update(R"([{"id":"root","component":"Column","children":["a","b"]},
    {"id":"a","component":"Text","text":{"@path":"/t"}},{"id":"b","component":"Text","text":"B"}])").c_str());
  // A view's object deleted from outside: the view goes inert, updates are safe.
  lv_obj_delete(f.obj("a"));
  CHECK(f.obj("a") == nullptr);
  f.send(R"({"version":"1.0","updateDataModel":{"surfaceId":"s","path":"/t","value":"two"}})");
  CHECK(label_text_is(f.obj("b"), "B"));
  // The surface container deleted from outside: the surface unmounts.
  lv_obj_delete(f.view->obj());
  CHECK(!f.view->mounted());
  CHECK(f.view->active_subscription_count() == 0);
  CHECK(f.processor->model().get_surface("s")->data_model().observer_count() == 0);
  f.send(R"({"version":"1.0","updateDataModel":{"surfaceId":"s","path":"/t","value":"three"}})");
  // deleteSurface after that is still fine.
  f.send(R"({"version":"1.0","deleteSurface":{"surfaceId":"s"}})");
  CHECK(f.view->model() == nullptr);
}

lv_point_t g_point;
bool g_pressed = false;

void pointer_read(lv_indev_t*, lv_indev_data_t* data) {
  data->point = g_point;
  data->state = g_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void tap(lv_obj_t* obj) {
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  g_point.x = (a.x1 + a.x2) / 2;
  g_point.y = (a.y1 + a.y2) / 2;
  g_pressed = true;
  lvgl_env::run(3, 20);
  g_pressed = false;
  lvgl_env::run(3, 20);
}

void test_pointer_input_and_self_removal() {
  lv_indev_t* indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, pointer_read);
  {
    Fixture f;
    f.send(kCreate);
    // The button hides itself through a local function call: its view is
    // removed while its own click handler runs.
    f.send(R"({"version":"1.0","updateDataModel":{"surfaceId":"s","path":"/items","value":[{"n":"one"}]}})");
    f.send(update(R"([{"id":"root","component":"Column","children":["go","list"]},
      {"id":"go","component":"Button","child":"go_t","action":{"event":{"name":"go","context":{"k":"v"}}}},
      {"id":"go_t","component":"Text","text":"Go"},
      {"id":"list","component":"List","children":{"componentId":"item","path":"/items"}},
      {"id":"item","component":"Button","child":"item_t","action":{"functionCall":{"@call":"formatString","args":{"value":"x"}}}},
      {"id":"item_t","component":"Text","text":{"@path":"n"}}])").c_str());
    tap(f.obj("go"));
    CHECK(f.actions.size() == 1);
    CHECK(!f.actions.empty() && f.actions[0]["name"].as_string() == "go");
    CHECK(!f.actions.empty() && f.actions[0]["context"]["k"].as_string() == "v");
    // Accessibility through the object seam: label inferred from the child Text.
    auto a = a2ui_lvgl::accessibility_of(f.obj("go"));
    CHECK(a && a->role == "button" && a->label == "Go");
    // Remove the list item while one of its objects is pressed (deletion during input).
    lv_obj_t* item = f.obj("list");
    CHECK(item && lv_obj_get_child_count(item) == 1);
    lv_area_t ar;
    lv_obj_get_coords(lv_obj_get_child(item, 0), &ar);
    g_point.x = (ar.x1 + ar.x2) / 2;
    g_point.y = (ar.y1 + ar.y2) / 2;
    g_pressed = true;
    lvgl_env::run(2, 20);
    f.send(R"({"version":"1.0","updateDataModel":{"surfaceId":"s","path":"/items","value":[]}})");
    g_pressed = false;
    lvgl_env::run(3, 20);
    CHECK(lv_obj_get_child_count(f.obj("list")) == 0);
  }
  lv_indev_delete(indev);
}

void test_modal_and_teardown() {
  Fixture f;
  f.send(kCreate);
  f.send(update(R"([{"id":"root","component":"Modal","trigger":"t","content":"c"},
    {"id":"t","component":"Button","child":"tt","action":{"event":{"name":"open"}}},
    {"id":"tt","component":"Text","text":"Open"},{"id":"c","component":"Text","text":"Body"}])").c_str());
  lv_obj_t* top = lv_layer_top();
  CHECK(lv_obj_get_child_count(top) == 0);
  CHECK(f.find("t")->simulate_gesture());
  CHECK(f.actions.size() == 1);
  CHECK(lv_obj_get_child_count(top) == 1);
  CHECK(f.find("c") != nullptr && f.obj("c") != nullptr);
  // Destroying the surface while the dialog is open cleans the top layer.
  f.view.reset();
  CHECK(lv_obj_get_child_count(top) == 0);
}

class Upper : public a2ui_lvgl::ComponentInstance {
 public:
  lv_obj_t* mount(a2ui_lvgl::NodeView&, lv_obj_t* parent) override { return label_ = lv_label_create(parent); }
  void update(a2ui_lvgl::NodeView& view) override {
    std::string t = view.string_prop("text");
    for (char& ch : t) ch = static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
    lv_label_set_text(label_, t.c_str());
  }

 private:
  lv_obj_t* label_ = nullptr;
};

void test_registry_override_and_markdown_text() {
  a2ui_lvgl::ComponentRegistry reg;
  a2ui_lvgl::register_basic_components(reg);
  reg.add({"Text", "", [] { return std::unique_ptr<a2ui_lvgl::ComponentInstance>(new Upper()); }});
  CHECK(reg.size() == 18);
  {
    a2ui_lvgl::SurfaceConfig cfg;
    cfg.registry = &reg;
    Fixture f(std::move(cfg));
    f.send(kCreate);
    f.send(update(R"([{"id":"root","component":"Text","text":"hello"}])").c_str());
    CHECK(label_text_is(f.obj("root"), "HELLO"));
  }
  // Basic Text: plain label, then Markdown (span group) in the same slot.
  Fixture f;
  f.send(kCreate);
  f.send(update(R"([{"id":"root","component":"Column","children":["a","t","z"]},
    {"id":"a","component":"Text","text":"first"},{"id":"t","component":"Text","text":"plain"},
    {"id":"z","component":"Text","text":"last"}])").c_str());
  lv_obj_t* col = f.obj("root");
  CHECK(lv_obj_check_type(f.obj("t"), &lv_label_class));
  const uint32_t serial = f.find("t")->mount_serial();
  f.send(update(R"([{"id":"t","component":"Text","text":"# Title\n\nSome **bold** and *it*\n\n- one\n- two"}])").c_str());
  CHECK(lv_obj_check_type(f.obj("t"), &lv_spangroup_class));
  CHECK(lv_obj_get_child(col, 1) == f.obj("t"));
  CHECK(f.find("t")->mount_serial() == serial);
  CHECK(f.find("t")->describe()["text"].as_string() == "Title\nSome bold and it\none\ntwo");
  f.send(update(R"([{"id":"t","component":"Text","text":"## Just a heading"}])").c_str());
  CHECK(lv_obj_check_type(f.obj("t"), &lv_label_class));
  CHECK(label_text_is(f.obj("t"), "Just a heading"));
  CHECK(lv_obj_get_child(col, 1) == f.obj("t"));
}

void test_live_region() {
  a2ui_lvgl::SurfaceConfig cfg;
  std::vector<std::string> said;
  cfg.services.announce = [&said](const std::string& text, bool assertive) {
    said.push_back(std::string(assertive ? "!" : "") + text);
  };
  Fixture f(std::move(cfg));
  f.send(kCreate);
  f.send(R"({"version":"1.0","updateDataModel":{"surfaceId":"s","path":"/m","value":"Saved"}})");
  f.send(update(R"([{"id":"root","component":"Text","text":{"@path":"/m"},"@accessibility":{"live":"assertive"}}])").c_str());
  CHECK(said.empty());
  f.send(R"({"version":"1.0","updateDataModel":{"surfaceId":"s","path":"/m","value":"Sync failed"}})");
  CHECK(said.size() == 1 && said[0] == "!Sync failed");
}

/** Delivers a 2x2 image when asked to, now or later. */
class FakeLoader : public a2ui_lvgl::ImageLoader {
 public:
  std::shared_ptr<void> load(const a2ui_lvgl::ImageRequest& request, a2ui_lvgl::ImageDone done) override {
    requests.push_back(request.url);
    if (request.url.find("sync") != std::string::npos) {
      done(&image());
    } else {
      pending.push_back(std::move(done));
    }
    return std::make_shared<int>(0);
  }
  static const lv_image_dsc_t& image() {
    static const uint8_t px[2 * 2 * 2] = {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF};
    static lv_image_dsc_t dsc = [] {
      lv_image_dsc_t d{};
      d.header.magic = LV_IMAGE_HEADER_MAGIC;
      d.header.cf = LV_COLOR_FORMAT_RGB565;
      d.header.w = 2;
      d.header.h = 2;
      d.header.stride = 4;
      d.data_size = sizeof(px);
      d.data = px;
      return d;
    }();
    return dsc;
  }
  std::vector<std::string> requests;
  std::vector<a2ui_lvgl::ImageDone> pending;
};

void test_image_loader_focus_and_validity() {
  FakeLoader loader;
  a2ui_lvgl::SurfaceConfig cfg;
  cfg.services.image_loader = &loader;
  std::vector<bool> focus;
  cfg.services.on_text_focus = [&focus](lv_obj_t*, bool focused) { focus.push_back(focused); };
  {
    Fixture f(std::move(cfg));
    f.send(kCreate);
    f.send(update(R"j([{"id":"root","component":"Column","children":["i1","i2","bad","tf","b"]},
      {"id":"i1","component":"Image","url":"https://x/sync.png"},
      {"id":"i2","component":"Image","url":"https://x/later.png","variant":"avatar"},
      {"id":"bad","component":"Image","url":"javascript:alert(1)"},
      {"id":"tf","component":"TextField","label":"Name","value":{"@path":"/name"},
       "checks":[{"condition":{"@call":"required","args":{"value":{"@path":"/name"}}},"message":"Required"}]},
      {"id":"b","component":"Button","child":"bt","action":{"event":{"name":"save"}},
       "checks":[{"condition":{"@call":"required","args":{"value":{"@path":"/name"}}},"message":"Required"}]},
      {"id":"bt","component":"Text","text":"Save"}])j").c_str());
    CHECK(loader.requests.size() == 2);  // The javascript: URL is never requested.
    CHECK(f.find("i1")->describe()["loaded"].as_bool());
    CHECK(!f.find("i2")->describe()["loaded"].as_bool());
    CHECK(f.find("bad")->describe()["url"].as_string().empty());
    CHECK(lv_obj_get_width(f.obj("i2")) == a2ui_lvgl::Theme::light().image_avatar);
    loader.pending[0](&FakeLoader::image());
    CHECK(f.find("i2")->describe()["loaded"].as_bool());
    // Invalid checks disable the button and show the error under the field.
    CHECK(lv_obj_has_state(f.obj("b"), LV_STATE_DISABLED));
    CHECK(f.find("b")->accessibility().disabled);
    CHECK(f.find("tf")->describe()["validationErrors"].size() == 1);
    CHECK(f.find("b")->simulate_gesture());
    CHECK(f.actions.empty());
    CHECK(f.find("tf")->simulate_input(Json("Ada")));
    lvgl_env::run(1);
    CHECK(!lv_obj_has_state(f.obj("b"), LV_STATE_DISABLED));
    CHECK(f.find("tf")->describe()["validationErrors"].size() == 0);
    CHECK(f.find("b")->simulate_gesture());
    CHECK(f.actions.size() == 1);
    // Focus hook: the app shows its on-screen keyboard here.
    lv_obj_t* ta = nullptr;
    for (uint32_t i = 0; i < lv_obj_get_child_count(f.obj("tf")); ++i) {
      lv_obj_t* c = lv_obj_get_child(f.obj("tf"), static_cast<int32_t>(i));
      if (lv_obj_check_type(c, &lv_textarea_class)) ta = c;
    }
    CHECK(ta != nullptr);
    if (ta) {
      lv_obj_send_event(ta, LV_EVENT_FOCUSED, nullptr);
      lv_obj_send_event(ta, LV_EVENT_DEFOCUSED, nullptr);
    }
    CHECK(focus.size() == 2 && focus[0] && !focus[1]);
    // A request answered after the view is gone is harmless.
    f.send(update(R"([{"id":"i2","component":"Image","url":"https://x/later2.png"}])").c_str());
  }
  CHECK(loader.pending.size() == 2);
  loader.pending[1](&FakeLoader::image());
}

void test_markdown_parser() {
  namespace md = a2ui_lvgl::markdown;
  CHECK(md::strip("This is *italic* and **bold** text.") == "This is italic and bold text.");
  CHECK(md::strip("# Header 1\n## Header 2") == "Header 1\nHeader 2");
  CHECK(md::strip("Visit [Docs](https://x.y/a_(b)) now") == "Visit Docs now");
  CHECK(md::strip("Look ![photo](p.png)") == "Look photo");
  CHECK(md::strip("Hello <span>world</span>") == "Hello <span>world</span>");
  CHECK(md::strip("snake_case_name and 2 * 3 * 4") == "snake_case_name and 2 * 3 * 4");
  CHECK(md::strip("Use `a*b*c` here") == "Use a*b*c here");
  CHECK(md::strip("\\*not em\\*") == "*not em*");
  CHECK(md::strip("line one\nline two") == "line one line two");
  CHECK(md::strip("```\ncode **x**\n```") == "code **x**");
  md::Document d = md::parse("1. one\n2. two\n   - nested");
  CHECK(d.blocks.size() == 3 && d.blocks[0].marker == "1." && d.blocks[2].level == 1);
  d = md::parse("***both***");
  CHECK(d.blocks.size() == 1 && d.blocks[0].runs.size() == 1 &&
        d.blocks[0].runs[0].style == (md::kStrong | md::kEmphasis));
  CHECK(md::parse("plain text").is_plain());
  CHECK(!md::parse("**x**").is_plain());
  d = md::parse("Title\n=====\nbody");
  CHECK(d.blocks.size() == 2 && d.blocks[0].kind == md::Block::Kind::Heading && d.blocks[0].level == 1);
}

void test_iso8601() {
  using a2ui_lvgl::basic::IsoDateTime;
  IsoDateTime v;
  CHECK(a2ui_lvgl::basic::parse_iso8601("2025-01-31T09:30:00", &v) && v.has_date && v.has_time && v.has_seconds);
  CHECK(a2ui_lvgl::basic::format_iso8601(v, true, true, &v) == "2025-01-31T09:30:00");
  CHECK(a2ui_lvgl::basic::parse_iso8601("2025-06-15", &v) && v.has_date && !v.has_time);
  CHECK(a2ui_lvgl::basic::format_iso8601(v, true, false, nullptr) == "2025-06-15");
  CHECK(!a2ui_lvgl::basic::parse_iso8601("2025-02-30", &v));
  CHECK(!a2ui_lvgl::basic::parse_iso8601("25:00", &v));
  CHECK(a2ui_lvgl::basic::parse_iso8601("2024-02-29T23:59+02:00", &v) && v.zone == "+02:00" && !v.has_seconds);
  IsoDateTime prev = v;
  v.hour = 7;
  CHECK(a2ui_lvgl::basic::format_iso8601(v, true, true, &prev) == "2024-02-29T07:59+02:00");
  CHECK(a2ui_lvgl::basic::parse_iso8601("08:05", &v) && v.has_time && !v.has_date);
  CHECK(a2ui_lvgl::basic::format_iso8601(v, false, true, &v) == "08:05");
  CHECK(a2ui_lvgl::basic::format_iso8601(v, false, true, nullptr) == "08:05:00");
  CHECK(a2ui_lvgl::basic::parse_iso8601("2025-01-01T08:00:00.250Z", &v) && v.fraction == "250" && v.zone == "Z");
}


// --- Row text wrapping, svgPath icons, DateTimeInput range -----------------------------

int32_t right_edge(lv_obj_t* o) {
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  return a.x2 + lv_obj_get_style_margin_right(o, LV_PART_MAIN);
}

void test_row_text_wraps() {
  Fixture f;
  f.send(kCreate);
  f.send(update(R"([{"id":"root","component":"Column","children":["r1","r2","r3"]},
    {"id":"r1","component":"Row","children":["i1","t1"]},
    {"id":"i1","component":"Icon","name":"mail"},
    {"id":"t1","component":"Text","text":"A long line of text that cannot fit beside the icon on a narrow screen and has to wrap"},
    {"id":"r2","component":"Row","children":["t2","t3"]},
    {"id":"t2","component":"Text","text":"First column of text that is long enough to need two lines here"},
    {"id":"t3","component":"Text","text":"Second, shorter text"},
    {"id":"r3","component":"Row","justify":"spaceBetween","children":["t4","t5"]},
    {"id":"t4","component":"Text","text":"Left"},{"id":"t5","component":"Text","text":"Right"}])").c_str());
  lv_obj_t* r1 = f.obj("r1");
  lv_obj_t* t1 = f.obj("t1");
  const int32_t line = lv_font_get_line_height(lv_obj_get_style_text_font(t1, LV_PART_MAIN));
  // The text fits beside the icon and wraps instead of overflowing.
  CHECK(right_edge(t1) <= right_edge(r1));
  CHECK(lv_obj_get_x(t1) > lv_obj_get_x(f.obj("i1")));
  CHECK(lv_obj_get_height(t1) >= 2 * line);
  CHECK(lv_obj_get_height(r1) >= lv_obj_get_height(t1));
  // Two texts share the overflow in proportion to their length; both fit.
  lv_obj_t* t2 = f.obj("t2");
  lv_obj_t* t3 = f.obj("t3");
  CHECK(right_edge(t3) <= right_edge(f.obj("r2")));
  CHECK(lv_obj_get_width(t2) > lv_obj_get_width(t3));
  CHECK(lv_obj_get_height(t2) >= 2 * line);
  // Short texts keep their natural width (justify still has free space to place).
  CHECK(lv_obj_get_height(f.obj("t4")) < 2 * line);
  CHECK(lv_obj_get_style_max_width(f.obj("t4"), LV_PART_MAIN) == lv_pct(100));
  CHECK(right_edge(f.obj("t5")) == right_edge(f.obj("r3")));
  // Shorter text later: the cap is lifted again.
  f.send(update(R"([{"id":"t1","component":"Text","text":"Short"}])").c_str());
  CHECK(lv_obj_get_style_max_width(t1, LV_PART_MAIN) == lv_pct(100));
  CHECK(lv_obj_get_height(t1) < 2 * line);
}

void test_svg_path_icon() {
  using a2ui_lvgl::basic::render_svg_path;
  std::vector<uint8_t> m;
  // A square, a ring (nonzero: the inner circle runs the other way) and every command.
  CHECK(render_svg_path("M4 4h16v16H4z", 24, &m) && m.size() == 24 * 24);
  CHECK(m[12 * 24 + 12] == 255 && m[2 * 24 + 2] == 0 && m[4 * 24 + 4] == 255);
  CHECK(render_svg_path("M12 2a10 10 0 1 0 0 20a10 10 0 1 0 0-20zm0 4a6 6 0 1 1 0 12A6 6 0 0 1 12 6z", 48, &m));
  CHECK(m[24 * 48 + 24] == 0 && m[24 * 48 + 6] == 255 && m[0] == 0);
  CHECK(render_svg_path("M2,2L22,2 22,22C15 22 9 22 2 22S2 10 2 2zm1 1q4 0 4 4t4 4", 24, &m));
  CHECK(render_svg_path("M1-1.5.5.5e1l10 10V20H1z", 24, &m));  // Packed numbers.
  CHECK(!render_svg_path("", 24, &m) && m.empty());
  CHECK(!render_svg_path("L10 10 20 20", 24, &m));  // Must start with a moveto.
  CHECK(!render_svg_path("M5 5", 24, &m));          // Nothing to fill.
  // Drawn up to the first error.
  CHECK(render_svg_path("M4 4h16v16H4z M x", 24, &m) && m[12 * 24 + 12] == 255);

  Fixture f;
  f.send(kCreate);
  f.send(R"({"version":"1.0","updateDataModel":{"surfaceId":"s","path":"/p","value":"M4 4h16v16H4z"}})");
  f.send(update(R"([{"id":"root","component":"Row","children":["a","b","c"]},
    {"id":"a","component":"Icon","name":{"svgPath":"M12 2L22 22H2z"}},
    {"id":"b","component":"Icon","name":{"svgPath":{"@path":"/p"}}},
    {"id":"c","component":"Icon","name":"home"}])").c_str());
  lv_obj_t* a = f.obj("a");
  CHECK(a != nullptr && !lv_obj_check_type(a, &lv_label_class));
  CHECK(lv_obj_get_width(a) == 24 && lv_obj_get_height(a) == 24);
  CHECK(f.find("a")->describe()["name"]["svgPath"].as_string() == "M12 2L22 22H2z");
  CHECK(f.find("b")->describe()["name"]["svgPath"].as_string() == "M4 4h16v16H4z");
  CHECK(label_text_is(f.obj("c"), LV_SYMBOL_HOME));
  f.send(R"({"version":"1.0","updateDataModel":{"surfaceId":"s","path":"/p","value":"M2 2h4v4H2z"}})");
  CHECK(f.find("b")->describe()["name"]["svgPath"].as_string() == "M2 2h4v4H2z");
  // A name switch swaps the object kind both ways; invalid data shows the fallback glyph.
  f.send(update(R"([{"id":"a","component":"Icon","name":"home"},
    {"id":"c","component":"Icon","name":{"svgPath":"nonsense"}}])").c_str());
  CHECK(label_text_is(f.obj("a"), LV_SYMBOL_HOME));
  CHECK(label_text_is(f.obj("c"), LV_SYMBOL_BULLET));
  CHECK(lv_obj_get_child_count(f.obj("root")) == 3);
}

lv_obj_t* find_in(lv_obj_t* parent, const lv_obj_class_t* cls) {
  for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i) {
    lv_obj_t* c = lv_obj_get_child(parent, static_cast<int32_t>(i));
    if (lv_obj_check_type(c, cls)) return c;
    if (lv_obj_t* found = find_in(c, cls)) return found;
  }
  return nullptr;
}

/** Button index of `day` in lv_calendar's matrix for March 2026 (1 March 2026 is a Sunday). */
uint32_t march_2026_button(int day) { return static_cast<uint32_t>(7 + day - 1); }

void test_date_time_range() {
  Fixture f;
  f.send(kCreate);
  f.send(R"({"version":"1.0","updateDataModel":{"surfaceId":"s","path":"/max","value":"2026-03-20"}})");
  a2ui::Status st = f.send(update(R"([{"id":"root","component":"Column","children":["d","t","dt"]},
    {"id":"d","component":"DateTimeInput","enableDate":true,"value":{"@path":"/d"},
     "min":"2026-03-10","max":{"@path":"/max"}},
    {"id":"t","component":"DateTimeInput","enableTime":true,"value":{"@path":"/t"},"min":"09:00","max":"17:30"},
    {"id":"dt","component":"DateTimeInput","enableDate":true,"enableTime":true,"value":{"@path":"/dt"},
     "min":"2026-03-10T08:30:00"}])").c_str());
  CHECK(st.ok());  // Literal min/max need the date/time formats to assert (schema.cpp).
  auto data = [&](const char* path) {
    auto r = f.processor->model().get_surface("s")->data_model().get(path);
    return r.ok() && r.value() && r.value()->is_string() ? r.value()->as_string() : std::string();
  };
  // Values picked outside the range are clamped to it.
  CHECK(f.find("d")->simulate_input(Json("2026-03-25")));
  CHECK(data("/d") == "2026-03-20");
  CHECK(f.find("d")->simulate_input(Json("2026-03-01")));
  CHECK(data("/d") == "2026-03-10");
  CHECK(f.find("d")->simulate_input(Json("2026-03-15")));
  CHECK(data("/d") == "2026-03-15");
  CHECK(f.find("t")->simulate_input(Json("20:15")));
  CHECK(data("/t") == "17:30:00");
  CHECK(f.find("t")->simulate_input(Json("06:00")));
  CHECK(data("/t") == "09:00:00");
  // On the first allowed day the time cannot be before the minimum.
  CHECK(f.find("dt")->simulate_input(Json("2026-03-10T07:00:00")));
  CHECK(data("/dt") == "2026-03-10T08:30:00");
  CHECK(f.find("dt")->simulate_input(Json("2026-03-11T07:00:00")));
  CHECK(data("/dt") == "2026-03-11T07:00:00");

  // The open picker disables days outside the range and the arrows past it.
  lv_obj_t* field = lv_obj_get_child(f.obj("d"), 1);
  lv_obj_send_event(field, LV_EVENT_CLICKED, nullptr);
  lvgl_env::run(1);
  lv_obj_t* cal = find_in(lv_layer_top(), &lv_calendar_class);
  CHECK(cal != nullptr);
  if (cal) {
    lv_obj_t* days = lv_calendar_get_btnmatrix(cal);
    CHECK(lv_buttonmatrix_has_button_ctrl(days, march_2026_button(9), LV_BUTTONMATRIX_CTRL_DISABLED));
    CHECK(!lv_buttonmatrix_has_button_ctrl(days, march_2026_button(10), LV_BUTTONMATRIX_CTRL_DISABLED));
    CHECK(!lv_buttonmatrix_has_button_ctrl(days, march_2026_button(20), LV_BUTTONMATRIX_CTRL_DISABLED));
    CHECK(lv_buttonmatrix_has_button_ctrl(days, march_2026_button(21), LV_BUTTONMATRIX_CTRL_DISABLED));
#if LV_USE_CALENDAR_HEADER_ARROW
    lv_obj_t* header = find_in(cal, &lv_calendar_header_arrow_class);
    CHECK(header && lv_obj_has_state(lv_obj_get_child(header, 0), LV_STATE_DISABLED));
    CHECK(header && lv_obj_has_state(lv_obj_get_child(header, 2), LV_STATE_DISABLED));
#endif
  }
  // The time rollers snap back into range.
  lv_obj_send_event(lv_obj_get_child(f.obj("t"), 1), LV_EVENT_CLICKED, nullptr);
  lv_obj_t* hours = find_in(lv_layer_top(), &lv_roller_class);
  CHECK(hours != nullptr);
  if (hours) {
    lv_roller_set_selected(hours, 22, LV_ANIM_OFF);
    lv_obj_send_event(hours, LV_EVENT_VALUE_CHANGED, nullptr);
    CHECK(lv_roller_get_selected(hours) == 17);
  }
}


void test_theme_emphasis_fonts() {
  // Bold and italic runs use the theme's fonts when it has them (docs/adapter.md, "Bold and italic").
  a2ui_lvgl::Theme theme = a2ui_lvgl::Theme::light();
  theme.bold = &lv_font_montserrat_20;
  theme.italic = &lv_font_montserrat_16;
  a2ui_lvgl::SurfaceConfig cfg;
  cfg.theme = &theme;
  Fixture f(std::move(cfg));
  f.send(kCreate);
  f.send(update(R"([{"id":"root","component":"Text","text":"plain **bold** *italic*"}])").c_str());
  lv_obj_t* spans = f.obj("root");
  CHECK(spans && lv_obj_check_type(spans, &lv_spangroup_class));
  if (!spans) return;
  const lv_font_t* bold = nullptr;
  const lv_font_t* italic = nullptr;
  for (uint32_t i = 0; i < lv_spangroup_get_span_count(spans); ++i) {
    lv_span_t* span = lv_spangroup_get_child(spans, static_cast<int32_t>(i));
    lv_style_value_t v;
    if (lv_style_get_prop(lv_span_get_style(span), LV_STYLE_TEXT_FONT, &v) != LV_STYLE_RES_FOUND) continue;
    if (std::strcmp(lv_span_get_text(span), "bold") == 0) bold = static_cast<const lv_font_t*>(v.ptr);
    if (std::strcmp(lv_span_get_text(span), "italic") == 0) italic = static_cast<const lv_font_t*>(v.ptr);
  }
  CHECK(bold == &lv_font_montserrat_20);
  CHECK(italic == &lv_font_montserrat_16);
}

}  // namespace

int main() {
  lvgl_env::init();
  {
    lvgl_env::Display disp;
    lvgl_env::create_display(&disp, 320, 480, LV_COLOR_FORMAT_RGB565, false);
    test_plain_lvgl();
    test_render_and_reorder();
    test_pending_takes_no_slot();
    test_fallbacks();
    test_external_deletion();
    test_pointer_input_and_self_removal();
    test_modal_and_teardown();
    test_registry_override_and_markdown_text();
    test_live_region();
    test_image_loader_focus_and_validity();
    test_markdown_parser();
    test_iso8601();
    test_row_text_wraps();
    test_svg_path_icon();
    test_date_time_range();
    test_theme_emphasis_fonts();
    lvgl_env::run(2);
    CHECK(lv_obj_get_child_count(lv_screen_active()) == 0);
    lv_display_delete(disp.display);
  }
  lvgl_env::deinit();

  if (g_failures != 0) {
    std::fprintf(stderr, "lvgl tests: %d of %d check(s) failed\n", g_failures, g_checks);
    return 1;
  }
  std::printf("lvgl tests: ok (%d checks)\n", g_checks);
  return 0;
}

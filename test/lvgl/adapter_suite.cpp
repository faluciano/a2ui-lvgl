// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Framework adapter conformance runner (conformance harness guide,
// "Framework adapter harness"): core/adapter_conformance.yaml
// (render_surface), core/accessibility.yaml (accessibility_check) and
// adapter/markdown_subset.yaml (render_markdown), against a2ui_lvgl::Surface
// mounted on a headless LVGL display.
//
// The view tree is read from the adapter, not the core: each NodeView is
// checked to own an LVGL object inside its parent's object (or none, for a
// node that renders nothing), and component values come from the widgets
// (ComponentInstance::describe reads label text, slider position, checkbox
// state, ...). User input drives the native controls (simulate_input /
// simulate_gesture send the LVGL events the widgets' handlers listen to).

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "a2ui/basic_catalog.h"
#include "a2ui/catalog.h"
#include "a2ui/data_context.h"
#include "a2ui/message_processor.h"
#include "a2ui/schema.h"
#include "a2ui/spec_documents.h"
#include "a2ui_lvgl/a2ui_lvgl.h"
#include "harness.h"
#include "lvgl_env.h"

namespace {

using a2ui::Catalog;
using a2ui::CatalogPtr;
using a2ui::ComponentNode;
using a2ui::MessageProcessor;
using a2ui::SurfaceModel;
using a2ui_lvgl::NodeView;
using harness::Json;
using harness::Outcome;

constexpr const char* kBadgeCatalog = "https://example.com/catalogs/badges/v1/catalog.json";

std::string str(const Json& j) { return j.is_string() ? j.as_string() : std::string(); }

// --- LVGL -------------------------------------------------------------------------

lvgl_env::Display& display() {
  static lvgl_env::Display d;
  static bool made = false;
  if (!made) {
    lvgl_env::init();
    lvgl_env::create_display(&d, 480, 800, LV_COLOR_FORMAT_RGB565, false);
    made = true;
  }
  return d;
}

void settle() {
  lvgl_env::run(3);
  lvgl_env::refresh();
}

// --- Catalogs and processor ---------------------------------------------------------

/** The basic catalog plus the harness functions (incrementCounter). */
CatalogPtr basic_catalog_for_tests() {
  auto base = Catalog::from_json(*a2ui::spec::parsed(a2ui::spec::Document::BasicCatalogV1));
  if (!base.ok()) return nullptr;
  CatalogPtr c = base.value();
  a2ui::basic_catalog::bind_functions(*c);
  a2ui::FunctionDecl inc;
  inc.api.name = "incrementCounter";
  inc.api.return_type = "void";
  inc.impl = [](const Json& args, a2ui::FunctionContext& fc) -> a2ui::Result<Json> {
    auto& dc = static_cast<a2ui::DataContext&>(fc);
    const std::string p = args["path"].is_string() ? args["path"].as_string() : "/count";
    const double step = args["step"].is_number() ? args["step"].as_number() : 1;
    const double cur = dc.model().value_at(p).as_number();
    a2ui::Status s = dc.model().set(p, Json(cur + step));
    if (!s.ok()) return s.error();
    return Json(nullptr);
  };
  c->add_function(std::move(inc));
  return c;
}

CatalogPtr inline_catalog(const Json& spec, std::string* error) {
  Json doc = Json::object();
  doc.set("catalogId", spec["catalogId"]);
  doc.set("protocolVersion", spec["protocolVersion"].is_string() ? spec["protocolVersion"] : Json("1.0"));
  if (spec.contains("components")) doc.set("components", spec["components"]);
  if (spec.contains("functions")) doc.set("functions", spec["functions"]);
  auto r = Catalog::from_json(doc);
  if (!r.ok()) {
    *error = r.error().message;
    return nullptr;
  }
  a2ui::basic_catalog::bind_functions(*r.value());
  return r.value();
}

/** StatusBadge from the multi-catalog case: a label showing badgeText. */
class StatusBadge : public a2ui_lvgl::ComponentInstance {
 public:
  lv_obj_t* mount(NodeView&, lv_obj_t* parent) override {
    label_ = lv_label_create(parent);
    lv_label_set_text_static(label_, "");
    return label_;
  }
  void update(NodeView& view) override { lv_label_set_text(label_, view.string_prop("badgeText").c_str()); }
  void describe(const NodeView&, Json* out) const override { out->set("badgeText", Json(lv_label_get_text(label_))); }

 private:
  lv_obj_t* label_ = nullptr;
};

const a2ui_lvgl::ComponentRegistry& test_registry() {
  static const a2ui_lvgl::ComponentRegistry r = [] {
    a2ui_lvgl::ComponentRegistry reg;
    a2ui_lvgl::register_basic_components(reg);
    a2ui_lvgl::ComponentImplementation badge;
    badge.name = "StatusBadge";
    badge.catalog_id = kBadgeCatalog;
    badge.create = [] { return std::make_unique<StatusBadge>(); };
    reg.add(std::move(badge));
    return reg;
  }();
  return r;
}

// --- Outbound schema ------------------------------------------------------------------

const a2ui::SchemaRegistry* outbound_schema(const harness::Context& ctx, const Json** schema) {
  static a2ui::SchemaRegistry registry;
  static std::shared_ptr<const Json> doc;
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    Json d = harness::load_json_file(ctx.repo_path("specification/v1_0/json/renderer_to_agent.json"));
    if (d.is_object()) {
      doc = std::make_shared<const Json>(std::move(d));
      registry.add("renderer_to_agent.json", doc);
      if (auto common = a2ui::spec::common_types("1.0")) registry.add("common_types.json", common);
    }
  }
  if (!doc) return nullptr;
  *schema = doc.get();
  return &registry;
}

// --- Session ---------------------------------------------------------------------------

struct Session {
  std::unique_ptr<MessageProcessor> processor;
  std::unique_ptr<a2ui_lvgl::Surface> view;
  lv_obj_t* host = nullptr;
  std::string surface_id;
  bool ever_mounted = false;
  // Captured during the current step.
  std::vector<Json> outbound;
  std::vector<Json> actions;
  std::vector<Json> errors;
  std::string last_call_id;
  std::string last_process_error;
  std::vector<a2ui::Subscription> subs;
  std::map<std::string, a2ui::Subscription> surface_subs;

  ~Session() {
    view.reset();
    surface_subs.clear();
    subs.clear();
    processor.reset();
    if (host) lv_obj_delete(host);
  }

  SurfaceModel* model() const {
    return processor && !surface_id.empty() ? processor->model().get_surface(surface_id) : nullptr;
  }

  void record_outbound(const Json& msg) {
    outbound.push_back(msg);
    const Json& call = msg["callAgentFunction"];
    if (call["functionCallId"].is_string()) last_call_id = call["functionCallId"].as_string();
  }

  void watch_surface(SurfaceModel* s) {
    s->set_url_handler([](const std::string&) { return a2ui::ok_status(); });  // openUrl stub.
    const std::string id = s->id();
    surface_subs[id + "#action"] = s->on_action().subscribe([this](const Json& a) {
      actions.push_back(a);
      Json wire = a;
      wire.erase("catalogId");  // Renderer-local.
      record_outbound(Json::object({{"version", Json("1.0")}, {"action", std::move(wire)}}));
    });
    surface_subs[id + "#error"] = s->on_error().subscribe([this](const Json& e) {
      errors.push_back(e);
      record_outbound(Json::object({{"version", Json("1.0")}, {"error", e}}));
    });
  }

  void mount_if_needed() {
    if (view || ever_mounted) return;
    SurfaceModel* m = nullptr;
    for (const auto& s : processor->model().surfaces()) {
      m = s.second.get();
      surface_id = s.first;
      break;
    }
    if (!m) return;
    a2ui_lvgl::SurfaceConfig config;
    config.registry = &test_registry();
    view = std::make_unique<a2ui_lvgl::Surface>(*m, host, config);
    ever_mounted = true;
  }
};

Json replace_refs(const Json& j, const Session& s, int depth = 0) {
  if (depth > 32) return j;
  if (j.is_object()) {
    if (j.size() == 1 && str(j["$ref"]) == "lastCallId") return Json(s.last_call_id);
    Json out = Json::object();
    for (const auto& m : j.as_object()) out.set(m.first, replace_refs(m.second, s, depth + 1));
    return out;
  }
  if (j.is_array()) {
    Json out = Json::array();
    for (const auto& e : j.as_array()) out.push_back(replace_refs(e, s, depth + 1));
    return out;
  }
  return j;
}

Outcome make_session(const Json& tc, const harness::Context& ctx, Session* s, bool relaxed_default) {
  (void)ctx;
  std::vector<CatalogPtr> catalogs;
  CatalogPtr basic = basic_catalog_for_tests();
  if (!basic) return Outcome::fail("basic catalog failed to load");
  catalogs.push_back(basic);
  for (const auto& spec : tc["catalogs"].as_array()) {
    std::string error;
    CatalogPtr c = inline_catalog(spec, &error);
    if (!c) return Outcome::fail("inline catalog: " + error);
    catalogs.push_back(c);
  }
  a2ui::MessageProcessorOptions options;
  bool relaxed = relaxed_default;
  if (tc["strictMode"].is_bool()) relaxed = !tc["strictMode"].as_bool();
  a2ui::ValidationConfig config = relaxed ? a2ui::ValidationConfig::relaxed() : a2ui::ValidationConfig::strict();
  // A rendered surface shows a cycle as a `cyclic` placeholder rather than
  // rejecting the message (as the SwiftUI harness configures its processor).
  if (relaxed) config.allow_cyclic_references = true;
  options.validation_config = config;
  options.outbound_listener = [s](const Json& m) { s->record_outbound(m); };
  s->processor = std::make_unique<MessageProcessor>(catalogs, options);
  s->subs.push_back(s->processor->model().on_surface_created().subscribe([s](SurfaceModel* m) { s->watch_surface(m); }));
  s->host = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(s->host);
  lv_obj_set_size(s->host, lv_pct(100), lv_pct(100));
  return Outcome::pass();
}

// --- View tree ---------------------------------------------------------------------------

bool occupies_layout(NodeView* v) { return v->obj() && !lv_obj_is_hidden(v->obj()); }

/** Checks that each child's object lives inside its parent's object (or in a popup on the top layer). */
std::string check_structure(NodeView* v) {
  for (NodeView* c : v->children()) {
    if (lv_obj_t* o = c->obj()) {
      NodeView* encl = a2ui_lvgl::enclosing_view(lv_obj_get_parent(o));
      if (encl != v && encl != nullptr) {
        return "view '" + c->node().component_id() + "' is rendered inside '" + encl->node().component_id() +
               "', not its parent '" + v->node().component_id() + "'";
      }
      if (!encl && v->node().type() != "Modal") {
        return "view '" + c->node().component_id() + "' is detached from its parent's objects";
      }
      if (a2ui_lvgl::view_of(o) != c) return "object of '" + c->node().component_id() + "' maps to another view";
    }
    std::string sub = check_structure(c);
    if (!sub.empty()) return sub;
  }
  return {};
}

Json view_json(NodeView* v) {
  const ComponentNode& n = v->node();
  Json out = Json::object();
  out.set("id", Json(n.component_id()));
  out.set("type", Json(n.type()));
  out.set("state", Json(a2ui::node_state_name(n.state())));
  out.set("occupiesLayout", Json(occupies_layout(v)));
  Json described = v->describe();
  for (const auto& m : described.as_object()) out.set(m.first, m.second);
  if (n.state() == a2ui::NodeState::Resolved) {
    out.set("isValid", Json(n.is_valid()));
    if (!out.contains("validationErrors")) out.set("validationErrors", n.validation_errors());
    out.set("validationResults", n.validation_results());
    if (!out.contains("hints")) out.set("hints", Json::array());
    // Custom props of components without a describe(): plain prop values.
    for (const auto& m : n.props()) {
      if (!out.contains(m.key) && (m.value.is_literal() || m.value.is_binding())) out.set(m.key, m.value.value());
    }
  }
  Json children = Json::array();
  for (NodeView* c : v->children()) children.push_back(view_json(c));
  out.set("children", std::move(children));
  return out;
}

std::string match_view(const Json& expected, const Json& actual, const std::string& path) {
  for (const auto& m : expected.as_object()) {
    if (m.first == "children") continue;
    if (!actual.contains(m.first)) return path + "." + m.first + ": missing (have " + actual.dump() + ")";
    std::string mm = harness::json_mismatch(m.second, actual[m.first], path + "." + m.first);
    if (!mm.empty()) return mm;
  }
  if (expected.contains("children")) {
    const auto& want = expected["children"].as_array();
    const auto& got = actual["children"].as_array();
    if (want.size() != got.size()) {
      std::string ids;
      for (const auto& g : got) ids += str(g["id"]) + " ";
      return path + ".children: expected " + std::to_string(want.size()) + ", got " + std::to_string(got.size()) +
             " [" + ids + "]";
    }
    for (size_t i = 0; i < want.size(); ++i) {
      std::string mm = match_view(want[i], got[i], path + "/children/" + std::to_string(i));
      if (!mm.empty()) return mm;
    }
  }
  return {};
}

NodeView* view_at(NodeView* root, const std::string& path) {
  if (!root) return nullptr;
  if (path == "root") return root;
  const std::string prefix = "root/children/";
  if (path.compare(0, prefix.size(), prefix) != 0) return nullptr;
  NodeView* v = root;
  size_t pos = 4;  // after "root"
  while (pos < path.size()) {
    if (path.compare(pos, 10, "/children/") != 0) return nullptr;
    pos += 10;
    size_t end = path.find('/', pos);
    if (end == std::string::npos) end = path.size();
    const size_t index = static_cast<size_t>(std::stoul(path.substr(pos, end - pos)));
    auto kids = v->children();
    if (index >= kids.size()) return nullptr;
    v = kids[index];
    pos = end;
  }
  return v;
}

struct Identity {
  NodeView* view = nullptr;
  uint32_t serial = 0;
  lv_obj_t* obj = nullptr;
};

void collect_views(NodeView* v, std::vector<NodeView*>* out) {
  if (!v) return;
  out->push_back(v);
  for (NodeView* c : v->children()) collect_views(c, out);
}

NodeView* find_view(NodeView* root, const std::string& id) {
  std::vector<NodeView*> all;
  collect_views(root, &all);
  for (NodeView* v : all) {
    if (v->node().component_id() == id) return v;
  }
  return nullptr;
}

std::string match_list(const Json& expected, const std::vector<Json>& actual, const char* what) {
  const auto& want = expected.as_array();
  if (want.size() != actual.size()) {
    std::string got;
    for (const auto& a : actual) got += a.dump() + " ";
    return std::string(what) + ": expected " + std::to_string(want.size()) + ", got " + std::to_string(actual.size()) +
           " " + got;
  }
  for (size_t i = 0; i < want.size(); ++i) {
    for (const auto& m : want[i].as_object()) {
      const Json& a = actual[i][m.first];
      if (m.first == "message" && m.second.is_string()) {
        if (!harness::regex_search(m.second.as_string(), str(a))) {
          return std::string(what) + "[" + std::to_string(i) + "].message: '" + str(a) + "' !~ " + m.second.dump();
        }
        continue;
      }
      std::string mm = harness::json_mismatch(m.second, a, std::string(what) + "[" + std::to_string(i) + "]." + m.first);
      if (!mm.empty()) return mm;
    }
  }
  return {};
}

std::string check_accessibility(NodeView* root, const Json& expected) {
  for (const auto& m : expected.as_object()) {
    NodeView* v = find_view(root, m.first);
    if (!v) return "accessibility: no view for '" + m.first + "'";
    // Read through the object seam where the view has an object.
    a2ui_lvgl::Accessibility a = v->accessibility();
    if (v->obj()) {
      auto via_obj = a2ui_lvgl::accessibility_of(v->obj());
      if (!via_obj) return "accessibility: object of '" + m.first + "' does not resolve";
      a = *via_obj;
    }
    Json got = Json::object({{"label", Json(a.label)},
                             {"description", Json(a.description)},
                             {"live", Json(a.live)},
                             {"hidden", Json(a.hidden)},
                             {"role", Json(a.role)}});
    if (a.checked) got.set("checked", Json(*a.checked));
    for (const auto& want : m.second.as_object()) {
      if (want.second.is_object() && want.second.contains("path")) continue;  // Pre-resolution binding maps.
      std::string mm = harness::json_mismatch(want.second, got[want.first], m.first + "." + want.first);
      if (!mm.empty()) return "accessibility: " + mm + " (got " + got.dump() + ")";
    }
  }
  return {};
}

std::string check_outbound(const Json& expected, const std::vector<Json>& actual, const harness::Context& ctx) {
  const Json* schema = nullptr;
  const a2ui::SchemaRegistry* reg = outbound_schema(ctx, &schema);
  for (const auto& env : actual) {
    if (!reg) break;
    a2ui::Status st = a2ui::validate_schema(*reg, "renderer_to_agent.json", *schema, env);
    if (!st.ok()) return "outbound envelope fails renderer_to_agent.json: " + st.error().message + " in " + env.dump();
  }
  const auto& want = expected.as_array();
  if (want.size() != actual.size()) {
    std::string got;
    for (const auto& a : actual) got += a.dump() + " ";
    return "outbound: expected " + std::to_string(want.size()) + ", got " + std::to_string(actual.size()) + " " + got;
  }
  for (size_t i = 0; i < want.size(); ++i) {
    std::string mm = harness::json_mismatch(want[i], actual[i], "outbound[" + std::to_string(i) + "]");
    if (!mm.empty()) return mm;
  }
  return {};
}

std::string check_top_layer_clean() {
  lv_obj_t* top = lv_display_get_layer_top(display().display);
  if (top && lv_obj_get_child_count(top) != 0) return "objects left on the top layer";
  return {};
}

// --- render_surface ----------------------------------------------------------------------

std::string run_step(Session& s, const Json& step, const harness::Context& ctx) {
  const std::string op = str(step["op"]);
  NodeView* root_before = s.view ? s.view->root_view() : nullptr;
  // Identity snapshot.
  std::map<std::string, Identity> before;
  for (const char* key : {"expectSameViewIdentity", "expectNewViewIdentity"}) {
    for (const auto& p : step[key].as_array()) {
      if (NodeView* v = view_at(root_before, str(p))) before[str(p)] = Identity{v, v->mount_serial(), v->obj()};
    }
  }
  std::map<std::string, std::vector<a2ui::NodePtr>> prior_nodes;
  if (step.contains("expectDisposedNodes")) {
    std::vector<NodeView*> all;
    collect_views(root_before, &all);
    for (NodeView* v : all) prior_nodes[v->node().component_id()].push_back(v->node_ptr());
  }
  s.outbound.clear();
  s.actions.clear();
  s.errors.clear();

  if (op == "process_messages") {
    Json messages = replace_refs(step["messages"], s);
    a2ui::Status st = s.processor->process_messages(messages);
    if (!st.ok()) s.last_process_error = st.error().message;
    s.mount_if_needed();
  } else if (op == "user_input" || op == "user_gesture" || op == "programmatic_invoke") {
    if (!s.view) return op + ": surface not mounted";
    NodeView* v = s.view->find_view(str(step["targetNode"]));
    if (!v) return op + ": no view for '" + str(step["targetNode"]) + "'";
    if (op == "user_input") {
      if (!v->simulate_input(step["inputValue"])) return "user_input: " + v->node().type() + " takes no input";
    } else if (op == "user_gesture") {
      if (!v->simulate_gesture()) return "user_gesture: " + v->node().type() + " has no gesture";
    } else {
      // An action invoked without a user gesture (no activation).
      SurfaceModel* m = s.model();
      const a2ui::PropValue* action = v->prop("action");
      if (!m || !action) return "programmatic_invoke: no action on '" + str(step["targetNode"]) + "'";
      Json resolved = m->resolve_action(action->value(), v->node().data_path(), false);
      if (resolved.is_object()) (void)m->dispatch_action(resolved, v->node().component_id());
    }
  } else if (op == "unmount_surface") {
    s.view.reset();
  } else {
    return "unknown op '" + op + "'";
  }
  settle();

  NodeView* root = s.view ? s.view->root_view() : nullptr;
  if (step.contains("expectViewTree")) {
    if (!root) return "expectViewTree: nothing rendered" + (s.last_process_error.empty() ? std::string() : " (processing failed: " + s.last_process_error + ")");
    std::string structure = check_structure(root);
    if (!structure.empty()) return structure;
    std::string mm = match_view(step["expectViewTree"], view_json(root), "root");
    if (!mm.empty()) return "expectViewTree: " + mm;
  }
  for (const auto& p : step["expectSameViewIdentity"].as_array()) {
    auto it = before.find(str(p));
    NodeView* now = view_at(root, str(p));
    if (it == before.end() || !now) return "expectSameViewIdentity: no view at " + str(p);
    if (now != it->second.view || now->mount_serial() != it->second.serial || now->obj() != it->second.obj) {
      return "expectSameViewIdentity: view at " + str(p) + " was remounted";
    }
  }
  for (const auto& p : step["expectNewViewIdentity"].as_array()) {
    auto it = before.find(str(p));
    NodeView* now = view_at(root, str(p));
    if (it == before.end() || !now) return "expectNewViewIdentity: no view at " + str(p);
    if (now == it->second.view && now->mount_serial() == it->second.serial) {
      return "expectNewViewIdentity: view at " + str(p) + " kept its identity";
    }
  }
  for (const auto& id : step["expectDisposedNodes"].as_array()) {
    auto it = prior_nodes.find(str(id));
    if (it == prior_nodes.end()) return "expectDisposedNodes: '" + str(id) + "' was not mounted before";
    std::vector<NodeView*> all;
    collect_views(root, &all);
    for (const auto& n : it->second) {
      if (!n->disposed()) return "expectDisposedNodes: '" + str(id) + "' is not disposed";
      for (NodeView* v : all) {
        if (v->node_ptr() == n) return "expectDisposedNodes: '" + str(id) + "' is still mounted";
      }
      if (n->props_signal().listener_count() != 0) return "expectDisposedNodes: '" + str(id) + "' keeps listeners";
    }
  }
  if (step["expectSurfaceRemoved"].as_bool()) {
    if (s.processor->model().get_surface(s.surface_id)) return "expectSurfaceRemoved: surface still in the group";
    if (s.view && s.view->mounted()) return "expectSurfaceRemoved: surface view still mounted";
  }
  if (step.contains("expectDataModel")) {
    SurfaceModel* m = s.model();
    if (!m) return "expectDataModel: no surface";
    std::string mm = harness::json_mismatch(step["expectDataModel"], m->data_model().root(), "dataModel");
    if (!mm.empty()) return "expectDataModel: " + mm + " (have " + m->data_model().root().dump() + ")";
  }
  if (step.contains("expectActions")) {
    std::string mm = match_list(step["expectActions"], s.actions, "expectActions");
    if (!mm.empty()) return mm;
  }
  if (step.contains("expectErrors")) {
    std::string mm = match_list(step["expectErrors"], s.errors, "expectErrors");
    if (!mm.empty()) return mm;
  }
  if (step.contains("expectAccessibilityTree")) {
    std::string mm = check_accessibility(root, step["expectAccessibilityTree"]);
    if (!mm.empty()) return mm;
  }
  if (step["expectThemeAbsent"].as_bool()) {
    SurfaceModel* m = s.model();
    if (!m) return "expectThemeAbsent: no surface";
    if (m->metadata().contains("theme")) return "expectThemeAbsent: the surface carries a theme";
    // The rendered surface uses the application theme only.
    const a2ui_lvgl::Theme app = a2ui_lvgl::Theme::light();
    if (!s.view || !lv_color_eq(lv_obj_get_style_bg_color(s.view->obj(), LV_PART_MAIN), app.background)) {
      return "expectThemeAbsent: surface background differs from the application theme";
    }
  }
  if (step.contains("expectActiveSubscriptions")) {
    size_t n = s.view ? s.view->active_subscription_count() : 0;
    if (SurfaceModel* m = s.model(); m && !s.view) n += m->data_model().observer_count();
    const size_t want = static_cast<size_t>(step["expectActiveSubscriptions"].as_number());
    if (n != want) {
      return "expectActiveSubscriptions: expected " + std::to_string(want) + ", got " + std::to_string(n);
    }
  }
  if (step.contains("expectOutbound")) {
    std::string mm = check_outbound(step["expectOutbound"], s.outbound, ctx);
    if (!mm.empty()) return mm;
  }
  return {};
}

HARNESS_ACTION("render_surface", run_render_surface) {
  if (harness::targets_pre_v1(test_case)) return Outcome::unsupported("pre-v1.0 surface");
  display();
  std::string failure;
  {
    Session s;
    // Relaxed unless the case asks for strict validation (as the SwiftUI
    // harness does): a rendered surface shows dangling references, unknown
    // types and cycles as placeholders instead of rejecting the message.
    Outcome o = make_session(test_case, ctx, &s, true);
    if (!o.passed()) return o;
    const auto& steps = test_case["steps"].as_array();
    for (size_t i = 0; i < steps.size() && failure.empty(); ++i) {
      std::string f = run_step(s, steps[i], ctx);
      if (!f.empty()) failure = "step " + std::to_string(i) + " (" + str(steps[i]["op"]) + "): " + f;
    }
  }
  settle();
  if (failure.empty()) failure = check_top_layer_clean();
  return failure.empty() ? Outcome::pass() : Outcome::fail(failure);
}

// --- accessibility_check -------------------------------------------------------------------

HARNESS_ACTION("accessibility_check", run_accessibility_check) {
  if (harness::targets_pre_v1(test_case)) return Outcome::unsupported("pre-v1.0 surface");
  display();
  std::string failure;
  int checked = 0;
  {
    Session s;
    Outcome o = make_session(test_case, ctx, &s, true);
    if (!o.passed()) return o;
    std::vector<Json> steps;
    if (test_case["steps"].is_array()) {
      for (const auto& st : test_case["steps"].as_array()) steps.push_back(st);
    } else {
      steps.push_back(Json::object({{"messages", test_case["messages"]}, {"assertions", test_case["assertions"]}}));
    }
    for (size_t i = 0; i < steps.size() && failure.empty(); ++i) {
      (void)s.processor->process_messages(steps[i]["messages"]);
      s.mount_if_needed();
      settle();
      NodeView* root = s.view ? s.view->root_view() : nullptr;
      if (!root) {
        failure = "nothing rendered";
        break;
      }
      const Json& tree = steps[i]["assertions"]["accessibilityTree"];
      std::string mm = check_accessibility(root, tree);
      if (!mm.empty()) failure = "step " + std::to_string(i) + ": " + mm;
      checked += static_cast<int>(tree.size());
    }
  }
  settle();
  if (failure.empty()) failure = check_top_layer_clean();
  if (!failure.empty()) return Outcome::fail(failure);
  if (checked == 0) return Outcome::fail("no accessibility node checked");
  // axeCore rules need a DOM; the attribute-level checks above still ran.
  return Outcome::pass(test_case.contains("assertions") && test_case["assertions"].contains("axeCore")
                           ? "axeCore UNSUPPORTED: no DOM"
                           : "");
}

// --- render_markdown -------------------------------------------------------------------------

std::string normalize_ws(const std::string& s) {
  std::string out;
  bool space = false;
  for (char c : s) {
    if (c == ' ' || c == '\n' || c == '\t') {
      space = !out.empty();
      continue;
    }
    if (space) out.push_back(' ');
    space = false;
    out.push_back(c);
  }
  return out;
}

HARNESS_ACTION("render_markdown", run_render_markdown) {
  display();
  // Render through a Text component and read the visible text back.
  const std::string md = str(test_case["markdown"]);
  Json messages = Json::array(
      {Json::object({{"version", Json("1.0")},
                     {"createSurface",
                      Json::object({{"surfaceId", Json("md")},
                                    {"catalogId", Json(a2ui::basic_catalog::kCatalogIdV1)},
                                    {"components", Json::array({Json::object({{"id", Json("root")},
                                                                               {"component", Json("Text")},
                                                                               {"text", Json(md)}})})}})}})});
  std::string failure;
  {
    Session s;
    Outcome o = make_session(test_case, ctx, &s, false);
    if (!o.passed()) return o;
    a2ui::Status st = s.processor->process_messages(messages);
    if (!st.ok()) return Outcome::fail("process: " + st.error().message);
    s.mount_if_needed();
    settle();
    NodeView* root = s.view ? s.view->root_view() : nullptr;
    if (!root) return Outcome::fail("nothing rendered");
    const std::string visible = normalize_ws(str(root->describe()["text"]));
    if (visible != str(test_case["expectVisibleText"])) {
      failure = "visible text '" + visible + "' != '" + str(test_case["expectVisibleText"]) + "'";
    }
    const a2ui_lvgl::markdown::Document doc = a2ui_lvgl::markdown::parse(md);
    std::set<std::string> present;
    if (doc.has_strong) present.insert("strong");
    if (doc.has_emphasis) present.insert("emphasis");
    if (doc.has_heading) present.insert("heading");
    if (doc.has_list) present.insert("list_item");
    if (doc.has_code) present.insert("code");
    // The renderer never produces html, image or link elements.
    for (const auto& f : test_case["expectPresent"].as_array()) {
      if (failure.empty() && !present.count(str(f))) failure = "missing " + str(f);
    }
    for (const auto& f : test_case["expectAbsent"].as_array()) {
      if (failure.empty() && present.count(str(f))) failure = "unexpected " + str(f);
    }
  }
  settle();
  return failure.empty() ? Outcome::pass() : Outcome::fail(failure);
}

}  // namespace

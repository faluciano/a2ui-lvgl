// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Runner for the `resolve_nodes` action (conformance/core/node_resolution.yaml,
// contract in node_resolution.md), ported from the Python reference runner
// python/a2ui_core/tests/node_resolution_runner.py.
//
// Fixtures live in conformance/test_data/node/*.yaml; the build converts
// them to JSON next to the suites.
//
// Protocol versions: this core implements v1.0 only. Fixtures on a v1.0
// catalog run as written. Fixtures on the v0.9.1 basic catalog exercise the
// same resolver behavior with v0.9 spellings, so they run on a mechanical
// v1.0 transliteration: the v1.0 basic catalog, {"path"} -> {"@path"},
// {"call"} -> {"@call"}, checks -> @checks, and "v0.9" message headers ->
// "1.0". Such a pass is reported with a note (visible with --verbose).

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "a2ui/catalog.h"
#include "a2ui/component_model.h"
#include "a2ui/message_processor.h"
#include "a2ui/node_resolver.h"
#include "a2ui/surface_model.h"
#include "harness.h"

namespace {

using a2ui::CatalogPtr;
using a2ui::ComponentNode;
using a2ui::NodePtr;
using a2ui::NodeResolver;
using a2ui::PropValue;
using a2ui::SurfaceModel;
using harness::Json;
using harness::Outcome;

// Upstream moved the v0.9.1 catalogs from specification/ to catalogs/ (#4150).
constexpr const char* kBasicV091 = "../catalogs/basic/v0_9_1/catalog.json";
constexpr const char* kBasicV091Old = "../specification/v0_9_1/catalogs/basic/catalog.json";
constexpr const char* kBasicV1 = "../catalogs/basic/v1/catalog.json";
constexpr const char* kBasicV1Id = "https://a2ui.org/specification/v1_0/catalogs/basic/catalog.json";

std::string str(const Json& j) { return j.is_string() ? j.as_string() : std::string(); }

/** Thrown-free assertion plumbing: the first failure is kept. */
struct Check {
  std::string failure;
  bool ok() const { return failure.empty(); }
  bool expect(bool cond, const std::string& why) {
    if (!cond && failure.empty()) failure = why;
    return cond;
  }
};

// --- v0.9 -> v1.0 transliteration ---------------------------------------------

Json upgrade_value(const Json& v, int depth = 0) {
  if (depth > 64) return v;
  if (v.is_array()) {
    Json out{Json::Array{}};
    for (const auto& e : v.as_array()) out.push_back(upgrade_value(e, depth + 1));
    return out;
  }
  if (!v.is_object()) return v;
  if (v.size() == 1 && v["path"].is_string()) return Json::object({{"@path", v["path"]}});
  Json out = Json::object();
  const bool is_call = v["call"].is_string() && !v.contains("componentId");
  for (const auto& m : v.as_object()) {
    if (is_call && m.first == "call") {
      out.set("@call", m.second);
    } else if (is_call && m.first == "returnType") {
      continue;
    } else {
      out.set(m.first, upgrade_value(m.second, depth + 1));
    }
  }
  return out;
}

/** A component definition (fixture or wire); `metadata_key` is its envelope key after upgrade. */
Json upgrade_component(const Json& c, const char* metadata_key) {
  Json out = Json::object();
  for (const auto& m : c.as_object()) {
    if (m.first == "checks") {
      out.set("@checks", upgrade_value(m.second));
    } else if (m.first == "accessibility") {
      out.set("@accessibility", upgrade_value(m.second));
    } else if (m.first == "metadata") {
      out.set(metadata_key, m.second);
    } else if (m.first == "id" || m.first == "component") {
      out.set(m.first, m.second);
    } else {
      out.set(m.first, upgrade_value(m.second));
    }
  }
  return out;
}

Json upgrade_components(const Json& comps, const char* metadata_key) {
  Json out{Json::Array{}};
  for (const auto& c : comps.as_array()) out.push_back(upgrade_component(c, metadata_key));
  return out;
}

bool is_pre_v1_header(const Json& msg) {
  const std::string v = a2ui::normalize_protocol_version(str(msg["version"]));
  return v == "0.9" || v == "0.8";
}

Json upgrade_message(const Json& msg) {
  Json out = msg;
  if (is_pre_v1_header(msg)) out.set("version", Json("1.0"));
  if (Json* cs = out.find_mut("createSurface"); cs && cs->is_object()) {
    cs->set("catalogId", Json(kBasicV1Id));
    if (cs->contains("components")) cs->set("components", upgrade_components((*cs)["components"], "@metadata"));
  }
  if (Json* uc = out.find_mut("updateComponents"); uc && uc->is_object()) {
    uc->set("components", upgrade_components((*uc)["components"], "@metadata"));
  }
  return out;
}

// --- Normalized views ----------------------------------------------------------------

Json normalize(const PropValue& v, const std::string& path) {
  switch (v.kind()) {
    case PropValue::Kind::Node:
      return Json::object({{"node", Json(path)}});
    case PropValue::Kind::Binding:
      return Json::object({{"value", v.value()}, {"writable", Json(v.writable())}});
    case PropValue::Kind::Action:
      return Json::object({{"action", Json(true)}});
    case PropValue::Kind::Array: {
      Json out{Json::Array{}};
      for (size_t i = 0; i < v.items().size(); ++i) out.push_back(normalize(v.items()[i], path + "/" + std::to_string(i)));
      return out;
    }
    case PropValue::Kind::Object: {
      Json out = Json::object();
      for (const auto& m : v.members()) out.set(m.key, normalize(m.value, path + "/" + m.key));
      return out;
    }
    case PropValue::Kind::Literal:
      break;
  }
  return v.value();
}

Json normalize_props(const a2ui::NodeProps& props, const std::string& path) {
  Json out = Json::object();
  for (const auto& m : props) out.set(m.key, normalize(m.value, path + "/" + m.key));
  return out;
}

/** Mounted positions ("root", "root/children/0") in visiting order. */
using Mounted = std::vector<std::pair<std::string, NodePtr>>;

const NodePtr* find_node(const Mounted& nodes, const std::string& path) {
  for (const auto& n : nodes) {
    if (n.first == path) return &n.second;
  }
  return nullptr;
}

bool mounted_contains(const Mounted& nodes, const ComponentNode* node) {
  for (const auto& n : nodes) {
    if (n.second.get() == node) return true;
  }
  return false;
}

void visit_children(const PropValue& v, const std::string& path, std::set<std::string>* sibling_ids, Mounted* out,
                    std::set<const ComponentNode*>* seen, Check* check);

void visit(const NodePtr& node, const std::string& path, Mounted* out, std::set<const ComponentNode*>* seen,
           Check* check) {
  check->expect(seen->insert(node.get()).second, path + ": each mounted position must have a distinct node");
  check->expect(!node->disposed(), path + ": mounted node is disposed");
  out->emplace_back(path, node);
  std::set<std::string> sibling_ids;
  for (const auto& m : node->props()) visit_children(m.value, path + "/" + m.key, &sibling_ids, out, seen, check);
}

void visit_children(const PropValue& v, const std::string& path, std::set<std::string>* sibling_ids, Mounted* out,
                    std::set<const ComponentNode*>* seen, Check* check) {
  if (v.is_node() && v.node_ptr()) {
    check->expect(sibling_ids->insert(v.node()->instance_id()).second, path + ": duplicate sibling instance_id");
    visit(v.node_ptr(), path, out, seen, check);
  } else if (v.is_array()) {
    for (size_t i = 0; i < v.items().size(); ++i) {
      visit_children(v.items()[i], path + "/" + std::to_string(i), sibling_ids, out, seen, check);
    }
  } else if (v.is_object()) {
    for (const auto& m : v.members()) visit_children(m.value, path + "/" + m.key, sibling_ids, out, seen, check);
  }
}

Mounted mounted_nodes(const NodeResolver* resolver, Check* check) {
  Mounted out;
  if (!resolver || !resolver->root()) return out;
  std::set<const ComponentNode*> seen;
  visit(resolver->root_node().value(), "root", &out, &seen, check);
  return out;
}

// --- Case state ------------------------------------------------------------------------

struct Watch {
  NodePtr node;  // Held so that node addresses are never reused within a case.
  std::string path;
  int emissions = 0;
  int metadata_emissions = 0;
  int destroyed = 0;
  Json last_props;
  Json last_metadata;
  std::vector<a2ui::Subscription> subs;
};

struct CaseState {
  const harness::Context* ctx = nullptr;
  bool upgraded = false;
  CatalogPtr catalog;
  std::unique_ptr<a2ui::MessageProcessor> processor;
  SurfaceModel* surface = nullptr;
  std::unique_ptr<NodeResolver> resolver;
  std::vector<std::unique_ptr<Watch>> watches;
  std::map<const ComponentNode*, int> created_emissions;
  std::vector<NodePtr> created_held;
  std::vector<a2ui::Subscription> created_subs;
  std::vector<a2ui::Subscription> case_subs;
  std::vector<Json> events, functions, errors;

  Watch* watch_of(const ComponentNode* n) {
    for (auto& w : watches) {
      if (w->node.get() == n) return w.get();
    }
    return nullptr;
  }

  void watch_nodes(const Mounted& nodes) {
    for (const auto& entry : nodes) {
      if (Watch* w = watch_of(entry.second.get())) {
        w->path = entry.first;
        continue;
      }
      auto w = std::make_unique<Watch>();
      Watch* wp = w.get();
      w->node = entry.second;
      w->path = entry.first;
      w->subs.push_back(entry.second->on_props_changed([this, wp](const a2ui::NodeProps& props) {
        ++wp->emissions;
        wp->last_props = normalize_props(props, wp->path);
        watch_created_nodes();
      }));
      w->subs.push_back(entry.second->metadata_signal().subscribe([wp](const Json& md) {
        ++wp->metadata_emissions;
        wp->last_metadata = md;
      }));
      w->subs.push_back(entry.second->on_destroyed().subscribe([wp]() { ++wp->destroyed; }));
      watches.push_back(std::move(w));
    }
  }

  void watch_created_nodes() {
    if (!resolver) return;
    Check ignore;
    for (const auto& entry : mounted_nodes(resolver.get(), &ignore)) {
      const ComponentNode* n = entry.second.get();
      if (created_emissions.count(n)) continue;
      created_emissions[n] = 0;
      created_held.push_back(entry.second);
      created_subs.push_back(entry.second->on_props_changed([this, n](const a2ui::NodeProps&) {
        ++created_emissions[n];
        watch_created_nodes();
      }));
    }
  }

  void attach_surface(SurfaceModel* s) {
    if (s->id() != "s" || resolver) return;
    surface = s;
    resolver = std::make_unique<NodeResolver>(*s);
    case_subs.push_back(resolver->root_node().subscribe([this](const NodePtr&) { watch_created_nodes(); }));
    case_subs.push_back(s->on_action().subscribe([this](const Json& action) {
      Json e = Json::object({{"name", action["name"]},
                             {"source_component_id", action["sourceComponentId"]},
                             {"context", action["context"].is_object() ? action["context"] : Json::object()}});
      if (!action["metadata"].is_nullish()) e.set("metadata", action["metadata"]);
      events.push_back(std::move(e));
    }));
    case_subs.push_back(s->on_error().subscribe([this](const Json& err) { errors.push_back(err); }));
  }

  void update_components(const Json& components) {
    for (const auto& raw : components.as_array()) {
      const Json comp = upgraded ? upgrade_component(raw, "metadata") : raw;
      const std::string id = str(comp["id"]);
      const std::string type = str(comp["component"]);
      Json metadata = comp.contains("metadata") ? comp["metadata"] : Json();
      Json props = Json::object();
      for (const auto& m : comp.as_object()) {
        if (m.first != "id" && m.first != "component" && m.first != "metadata") props.set(m.first, m.second);
      }
      if (a2ui::ComponentModel* existing = surface->components_model().get(id)) {
        existing->apply_update(std::move(props), std::move(metadata));
      } else {
        (void)surface->components_model().add_component(
            std::make_unique<a2ui::ComponentModel>(id, type, catalog, std::move(props), std::move(metadata)));
      }
    }
  }

  ~CaseState() {
    created_subs.clear();
    if (resolver) resolver->dispose();
    case_subs.clear();
    for (auto& w : watches) w->subs.clear();
    resolver.reset();
    processor.reset();
  }
};

// --- Assertions -----------------------------------------------------------------------

void check_nodes(const Mounted& nodes, const Json& expected, const std::string& reason, Check* check) {
  const Json& exp_nodes = expected["nodes"];
  std::set<std::string> want, got;
  for (const auto& m : exp_nodes.as_object()) want.insert(m.first);
  for (const auto& n : nodes) got.insert(n.first);
  if (want != got) {
    std::string w, g;
    for (const auto& s : want) w += s + " ";
    for (const auto& s : got) g += s + " ";
    check->expect(false, reason + ": node paths [" + g + "] != [" + w + "]");
    return;
  }
  static const std::set<std::string> kFields = {"component_id", "type", "state", "data_path", "metadata",
                                                "isValid", "validationErrors", "validationResults", "props"};
  for (const auto& m : exp_nodes.as_object()) {
    const std::string& path = m.first;
    const Json& fields = m.second;
    const NodePtr& node = *find_node(nodes, path);
    for (const auto& f : fields.as_object()) {
      if (!check->expect(kFields.count(f.first) > 0, reason + ": unknown node key '" + f.first + "'")) return;
    }
    const Json actual = Json::object({
        {"component_id", Json(node->component_id())},
        {"type", Json(node->type())},
        {"state", Json(a2ui::node_state_name(node->state()))},
        {"data_path", Json(node->data_path())},
        {"metadata", node->metadata().is_undefined() ? Json(nullptr) : node->metadata()},
        {"isValid", Json(node->is_valid())},
        {"validationErrors", node->validation_errors()},
        {"validationResults", node->validation_results()},
    });
    for (const auto& f : fields.as_object()) {
      if (f.first == "props") continue;
      check->expect(actual[f.first] == f.second, reason + ": " + path + "." + f.first + " expected " +
                                                     f.second.dump() + ", got " + actual[f.first].dump());
    }
    for (const auto& p : fields["props"].as_object()) {
      const PropValue* v = node->prop(p.first);
      if (!check->expect(v != nullptr, reason + ": " + path + "." + p.first + " is absent")) continue;
      const Json norm = normalize(*v, path + "/" + p.first);
      check->expect(norm == p.second, reason + ": " + path + "." + p.first + " expected " + p.second.dump() +
                                          ", got " + norm.dump());
    }
  }
}

Json counts_json(const std::map<std::string, int>& m) {
  Json out = Json::object();
  for (const auto& e : m) out.set(e.first, Json(e.second));
  return out;
}

void check_step_state(CaseState& cs, const Json& expected, const Mounted& before, const std::string& reason,
                      Check* check) {
  static const std::set<std::string> kKeys = {"created_emissions", "nodes",          "emissions", "metadata_emissions",
                                              "destroyed",         "same_nodes",     "replaced_nodes",
                                              "data",              "events",         "functions", "errors"};
  for (const auto& k : expected.as_object()) {
    if (!check->expect(kKeys.count(k.first) > 0, reason + ": unknown expect key '" + k.first + "'")) return;
  }
  const Mounted after = mounted_nodes(cs.resolver.get(), check);
  if (!check->ok()) return;
  check_nodes(after, expected, reason, check);
  if (!check->ok()) return;

  std::map<std::string, int> emissions, metadata_emissions, destroyed;
  for (const auto& w : cs.watches) {
    const bool surviving = mounted_contains(after, w->node.get());
    if (w->emissions && surviving) {
      emissions[w->path] += w->emissions;
      check->expect(w->last_props == normalize_props(w->node->props(), w->path), reason + ": " + w->path +
                                                                                   " last emission");
    }
    if (w->metadata_emissions && surviving) {
      metadata_emissions[w->path] += w->metadata_emissions;
      check->expect(w->last_metadata == w->node->metadata(), reason + ": " + w->path + " last metadata emission");
    }
    if (w->destroyed) {
      destroyed[w->path] += w->destroyed;
      check->expect(w->node->disposed(), reason + ": " + w->path + " destruction without disposal");
    }
  }
  const Json exp_emissions = expected["emissions"].is_object() ? expected["emissions"] : Json::object();
  check->expect(counts_json(emissions) == exp_emissions,
                reason + ": emissions expected " + exp_emissions.dump() + ", got " + counts_json(emissions).dump());
  if (expected.contains("created_emissions")) {
    std::map<std::string, int> actual;
    for (const auto& n : after) {
      bool existed = false;
      for (const auto& b : before) existed = existed || b.second == n.second;
      if (existed) continue;
      auto it = cs.created_emissions.find(n.second.get());
      actual[n.first] = it == cs.created_emissions.end() ? 0 : it->second;
    }
    const Json want = expected["created_emissions"].is_object() ? expected["created_emissions"] : Json::object();
    check->expect(counts_json(actual) == want, reason + ": created_emissions expected " + want.dump() + ", got " +
                                                   counts_json(actual).dump());
  }
  if (expected.contains("metadata_emissions")) {
    const Json want = expected["metadata_emissions"].is_object() ? expected["metadata_emissions"] : Json::object();
    check->expect(counts_json(metadata_emissions) == want, reason + ": metadata_emissions expected " + want.dump() +
                                                               ", got " + counts_json(metadata_emissions).dump());
  }
  const Json exp_destroyed = expected["destroyed"].is_object() ? expected["destroyed"] : Json::object();
  check->expect(counts_json(destroyed) == exp_destroyed,
                reason + ": destroyed expected " + exp_destroyed.dump() + ", got " + counts_json(destroyed).dump());

  for (const auto& p : expected["same_nodes"].as_array()) {
    const NodePtr* a = find_node(after, str(p));
    const NodePtr* b = find_node(before, str(p));
    check->expect(a && b && *a == *b, reason + ": " + str(p) + " identity");
  }
  for (const auto& p : expected["replaced_nodes"].as_array()) {
    const NodePtr* a = find_node(after, str(p));
    const NodePtr* b = find_node(before, str(p));
    check->expect(a && b && *a != *b, reason + ": " + str(p) + " replacement");
  }
  for (const auto& d : expected["data"].as_object()) {
    if (!check->expect(cs.surface != nullptr, reason + ": no surface for data check")) break;
    Json v = cs.surface->data_model().value_at(d.first);
    if (v.is_undefined()) v = Json(nullptr);
    check->expect(v == d.second, reason + ": data " + d.first + " expected " + d.second.dump() + ", got " + v.dump());
  }
  const Json exp_events = expected["events"].is_array() ? expected["events"] : Json(Json::Array{});
  Json events{Json::Array(cs.events)};
  check->expect(events == exp_events, reason + ": events expected " + exp_events.dump() + ", got " + events.dump());
  const Json exp_functions = expected["functions"].is_array() ? expected["functions"] : Json(Json::Array{});
  Json functions{Json::Array(cs.functions)};
  check->expect(functions == exp_functions,
                reason + ": functions expected " + exp_functions.dump() + ", got " + functions.dump());
  if (expected.contains("errors")) {
    const Json& exp_errors = expected["errors"];
    if (check->expect(exp_errors.size() == cs.errors.size(),
                      reason + ": errors count expected " + std::to_string(exp_errors.size()) + ", got " +
                          std::to_string(cs.errors.size()))) {
      for (size_t i = 0; i < exp_errors.size(); ++i) {
        for (const auto& kv : exp_errors[i].as_object()) {
          check->expect(cs.errors[i][kv.first] == kv.second,
                        reason + ": error." + kv.first + " expected " + kv.second.dump() + ", got " +
                            cs.errors[i][kv.first].dump());
        }
      }
    }
  }
}

Outcome run_step(CaseState& cs, const Json& step, const Mounted& before, const std::string& reason) {
  const std::string op = str(step["op"]);
  if (op == "set_data") {
    if (!cs.surface) return Outcome::fail(reason + ": no surface");
    (void)cs.surface->data_model().set(str(step["path"]), step["value"]);
  } else if (op == "update_components") {
    if (!cs.surface) return Outcome::fail(reason + ": no surface");
    cs.update_components(step["components"]);
  } else if (op == "remove_component") {
    if (!cs.surface) return Outcome::fail(reason + ": no surface");
    cs.surface->components_model().remove_component(str(step["component_id"]));
  } else if (op == "process_messages") {
    Json msgs = step["messages"];
    if (cs.upgraded) {
      Json up{Json::Array{}};
      for (const auto& m : msgs.as_array()) up.push_back(upgrade_message(m));
      msgs = std::move(up);
    }
    a2ui::Status s = cs.processor->process_messages(msgs);
    if (!s.ok()) return Outcome::fail(reason + ": processing failed: " + s.error().message);
  } else if (op == "write" || op == "invoke") {
    const NodePtr* node = find_node(before, str(step["node"]));
    if (!node) return Outcome::fail(reason + ": no node at " + str(step["node"]));
    const PropValue* v = (*node)->prop(str(step["property"]));
    if (op == "write") {
      if (!v || !v->writable()) return Outcome::fail(reason + ": not a writable binding");
      const PropValue binding = *v;  // The props may be replaced by the write.
      a2ui::Status s = (*node)->write(binding, step["value"]);
      if (!s.ok()) return Outcome::fail(reason + ": write failed: " + s.error().message);
    } else {
      if (!v || !v->is_action()) return Outcome::fail(reason + ": not an action");
      const PropValue action = *v;
      a2ui::Status s = (*node)->invoke(action, step.contains("metadata") ? step["metadata"] : Json());
      if (!s.ok()) return Outcome::fail(reason + ": invoke failed: " + s.error().message);
    }
  } else if (op == "dispose") {
    if (!cs.resolver) return Outcome::fail(reason + ": no resolver");
    cs.resolver->dispose();
  } else {
    return Outcome::fail(reason + ": unknown node operation " + op);
  }
  return Outcome::pass();
}

std::vector<std::pair<PropValue, Json>> save_bindings(const Mounted& nodes) {
  std::vector<std::pair<PropValue, Json>> saved;
  std::vector<const PropValue*> stack;
  for (const auto& n : nodes) {
    for (const auto& m : n.second->props()) stack.push_back(&m.value);
  }
  while (!stack.empty()) {
    const PropValue* v = stack.back();
    stack.pop_back();
    if (v->is_binding()) saved.emplace_back(*v, v->value());
    for (const auto& item : v->items()) stack.push_back(&item);
    for (const auto& m : v->members()) stack.push_back(&m.value);
  }
  return saved;
}

}  // namespace

HARNESS_ACTION("resolve_nodes", run_resolve_nodes) {
  const std::string name = str(test_case["name"]);
  std::string load_error;
  const Json fixture = harness::load_json_file(ctx.converted_path(str(test_case["fixture"])), &load_error);
  if (!fixture.is_object()) return Outcome::fail("fixture: " + load_error);

  CaseState cs;
  cs.ctx = &ctx;
  std::string catalog_rel = str(fixture["catalog"]);
  Json catalog_doc = harness::load_json_file(ctx.conformance_path(catalog_rel), &load_error);
  if (!catalog_doc.is_object()) return Outcome::fail("catalog: " + load_error);
  const std::string version = a2ui::normalize_protocol_version(str(catalog_doc["protocolVersion"]));
  if (version != "1.0") {
    if (catalog_rel != kBasicV091 && catalog_rel != kBasicV091Old) return Outcome::unsupported("fixture catalog targets protocol " + version);
    cs.upgraded = true;
    catalog_rel = kBasicV1;
    catalog_doc = harness::load_json_file(ctx.conformance_path(catalog_rel), &load_error);
    if (!catalog_doc.is_object()) return Outcome::fail("catalog: " + load_error);
  }
  auto loaded = harness::load_catalog(catalog_doc);
  if (!loaded.ok()) return Outcome::fail("catalog load failed: " + loaded.error().message);
  cs.catalog = loaded.value();
  // Declared functions are recorders returning null.
  for (const auto& fn : std::vector<a2ui::FunctionDecl>(cs.catalog->functions())) {
    const std::string fn_name = fn.api.name;
    CaseState* state = &cs;
    (void)cs.catalog->bind_function(
        fn_name, [state, fn_name](const Json& args, a2ui::FunctionContext&) -> a2ui::Result<Json> {
          state->functions.push_back(Json::object({{"name", Json(fn_name)}, {"args", args}}));
          return Json(nullptr);
        });
  }

  cs.processor = std::make_unique<a2ui::MessageProcessor>(std::vector<CatalogPtr>{cs.catalog});
  cs.case_subs.push_back(
      cs.processor->model().on_surface_created().subscribe([&cs](SurfaceModel* const& s) { cs.attach_surface(s); }));
  cs.case_subs.push_back(cs.processor->model().on_surface_deleted().subscribe([&cs](const std::string& id) {
    if (id == "s") cs.surface = nullptr;
  }));

  if (!fixture["surface"].is_bool() || fixture["surface"].as_bool()) {
    a2ui::SurfaceOptions options;
    options.protocol_version = cs.catalog->protocol_version();
    a2ui::CatalogMap available{{cs.catalog->id(), cs.catalog}};
    auto seeded = std::make_unique<SurfaceModel>("s", cs.catalog, std::move(available), std::move(options));
    (void)seeded->data_model().set("/", fixture["data"].is_object() ? fixture["data"] : Json::object());
    cs.surface = seeded.get();
    cs.update_components(fixture["components"]);
    cs.surface = nullptr;
    a2ui::Status s = cs.processor->model().add_surface(std::move(seeded));
    if (!s.ok()) return Outcome::fail("seeding failed: " + s.error().message);
  }

  Check check;
  check_step_state(cs, test_case["expect"], {}, name + " initial", &check);
  if (!check.ok()) return Outcome::fail(check.failure);

  const auto& steps = test_case["steps"].as_array();
  for (size_t i = 0; i < steps.size(); ++i) {
    const Json& step = steps[i];
    const std::string reason = name + " step " + std::to_string(i) + " (" + str(step["op"]) + ")";
    const Mounted before = mounted_nodes(cs.resolver.get(), &check);
    cs.watch_nodes(before);
    for (auto& w : cs.watches) {
      w->emissions = w->metadata_emissions = w->destroyed = 0;
      w->last_props = Json();
      w->last_metadata = Json();
    }
    cs.created_subs.clear();
    cs.created_held.clear();
    cs.created_emissions.clear();
    cs.events.clear();
    cs.functions.clear();
    cs.errors.clear();
    const auto bindings = save_bindings(before);

    Outcome o = run_step(cs, step, before, reason);
    if (!o.passed()) return o;
    check_step_state(cs, step["expect"], before, reason, &check);
    for (const auto& b : bindings) check.expect(b.first.value() == b.second, reason + ": prior binding snapshot changed");
    if (!check.ok()) return Outcome::fail(check.failure);
  }

  // Disposal releases everything the resolver held on the surface.
  if (cs.resolver) {
    cs.resolver->dispose();
    if (cs.resolver->active_subscription_count() != 0 || cs.resolver->node_count() != 0) {
      return Outcome::fail("resolver left " + std::to_string(cs.resolver->active_subscription_count()) +
                           " subscriptions and " + std::to_string(cs.resolver->node_count()) + " nodes");
    }
  }
  return Outcome::pass(cs.upgraded ? "v0.9.1 fixture run on its v1.0 transliteration" : "");
}

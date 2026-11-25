// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Runners for core/catalog.yaml (from_json, to_json, round_trip, derive,
// validation_schema), core/common_types.yaml (validate_common_type,
// common_types_schema) and core/agent_to_renderer.yaml
// (agent_to_renderer_schema). Semantics follow the Python reference
// harness (python/a2ui_core/tests/test_conformance.py).
//
// Only protocol v1.0 is implemented; cases for earlier versions are
// reported as unsupported.

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "a2ui/catalog.h"
#include "a2ui/schema.h"
#include "a2ui/spec_documents.h"
#include "a2ui/validation.h"
#include "harness.h"

namespace {

using a2ui::Catalog;
using a2ui::CatalogPtr;
using harness::Json;
using harness::Outcome;

// --- Case helpers -----------------------------------------------------------

std::string str(const Json& j) { return j.is_string() ? j.as_string() : std::string(); }

/** The case's catalog document (catalogPath/catalogFile, else inline). */
Json case_catalog(const Json& tc, const harness::Context& ctx, std::string* error) {
  std::string path = str(tc["catalogPath"]);
  if (path.empty()) path = str(tc["catalogFile"]);
  if (!path.empty()) return harness::load_json_file(ctx.repo_path(path), error);
  for (const char* k : {"catalogSchema", "catalog", "schema"}) {
    if (tc[k].is_object()) return tc[k];
  }
  return tc;
}

/** The protocol version a case targets, normalized; "" when it names none. */
std::string case_version(const Json& tc, const Json& document) {
  std::string v = str(tc["protocolVersion"]);
  if (v.empty() && document.is_object()) v = str(document["protocolVersion"]);
  return v.empty() ? std::string() : a2ui::normalize_protocol_version(v);
}

/** Sorts `enum` values and `required` names, whose order has no meaning. */
Json normalize_set_keywords(const Json& node) {
  if (node.is_array()) {
    Json out = Json::array();
    for (const auto& e : node.as_array()) out.push_back(normalize_set_keywords(e));
    return out;
  }
  if (!node.is_object()) return node;
  Json out = Json::object();
  for (const auto& m : node.as_object()) out.set(m.first, normalize_set_keywords(m.second));
  for (const char* k : {"enum", "required"}) {
    Json* v = out.find_mut(k);
    if (!v || !v->is_array()) continue;
    Json::Array items = v->as_array();
    std::sort(items.begin(), items.end(),
              [](const Json& a, const Json& b) { return a.dump() < b.dump(); });
    *v = Json(std::move(items));
  }
  return out;
}

std::string compare_documents(const Json& expected, const Json& actual) {
  return harness::json_mismatch(normalize_set_keywords(expected), normalize_set_keywords(actual));
}

void collect_refs(const Json& node, std::vector<std::string>* refs) {
  if (node.is_object()) {
    if (node["$ref"].is_string()) refs->push_back(node["$ref"].as_string());
    for (const auto& m : node.as_object()) collect_refs(m.second, refs);
  } else if (node.is_array()) {
    for (const auto& e : node.as_array()) collect_refs(e, refs);
  }
}

std::string self_contained_problem(const Json& schema) {
  std::vector<std::string> refs;
  collect_refs(schema, &refs);
  if (refs.empty()) return "catalog schema contains no references at all";
  for (const auto& r : refs) {
    if (r.empty() || r[0] != '#') return "reference '" + r + "' leaves the catalog document";
    if (!a2ui::resolve_json_pointer(schema, std::string_view(r).substr(1))) {
      return "reference '" + r + "' does not resolve within the catalog document";
    }
  }
  return "";
}

// Property and required names of a component schema, through allOf and
// local references (Python `_schema_shape`).
void schema_shape(const Json& doc, const Json& schema, std::set<std::string>* props,
                  std::set<std::string>* req, int depth) {
  if (!schema.is_object() || depth > 16) return;
  for (const auto& m : schema["properties"].as_object()) props->insert(m.first);
  for (const auto& r : schema["required"].as_array()) req->insert(str(r));
  const std::string ref = str(schema["$ref"]);
  if (!ref.empty() && ref[0] == '#') {
    if (const Json* t = a2ui::resolve_json_pointer(doc, std::string_view(ref).substr(1))) {
      schema_shape(doc, *t, props, req, depth + 1);
    }
  }
  for (const auto& m : schema["allOf"].as_array()) schema_shape(doc, m, props, req, depth + 1);
}

Json component_shapes(const Catalog& c) {
  const Json doc = c.validation_schema();
  Json out = Json::object();
  for (const auto& m : doc["components"].as_object()) {
    std::set<std::string> props, req;
    schema_shape(doc, m.second, &props, &req, 0);
    Json p = Json::array(), r = Json::array();
    for (const auto& n : props) {
      if (n != "id" && n != "component") p.push_back(n);
    }
    for (const auto& n : req) {
      if (n != "id" && n != "component") r.push_back(n);
    }
    out.set(m.first, Json::object({{"properties", p}, {"required", r}}));
  }
  return out;
}

Json function_signatures(const Catalog& c) {
  Json out = Json::object();
  for (const auto& f : c.functions()) {
    if (!f.api.name.empty() && f.api.name[0] == '@') continue;
    const char* callers = f.api.allowed_callers == a2ui::AllowedCallers::AgentOnly ? "agentOnly"
                          : f.api.allowed_callers == a2ui::AllowedCallers::RendererOrAgent
                              ? "rendererOrAgent"
                              : "rendererOnly";
    out.set(f.api.name, Json::object({{"returnType", Json(f.api.return_type)},
                                      {"allowedCallers", Json(callers)},
                                      {"requiresUserActivation", Json(f.api.requires_user_activation)}}));
  }
  return out;
}

/** The SDK's own basic catalog: the embedded published v1 document. */
CatalogPtr builtin_basic_catalog() {
  auto r = harness::load_basic_catalog();
  return r.ok() ? r.value() : nullptr;
}

Outcome check_component_sample(const Catalog& cat, const Json& comp, bool expect_valid) {
  const std::string type = str(comp["component"]);
  const a2ui::ComponentApi* api = cat.find_component(type);
  a2ui::Status s = api ? a2ui::validate_component_properties(cat, *api, comp)
                       : a2ui::Status(a2ui::make_error(a2ui::ErrorKind::Validation,
                                                       "Unknown component type '" + type + "'"));
  if (expect_valid && !s.ok()) {
    return Outcome::fail("expected valid component " + comp.dump() + ": " + s.error().message);
  }
  if (!expect_valid) {
    if (s.ok()) return Outcome::fail("expected invalid component " + comp.dump() + " to be rejected");
    if (!s.error().is(a2ui::ErrorKind::Validation)) {
      return Outcome::fail("expected a ValidationError for " + comp.dump() + ", got " +
                           a2ui::error_kind_name(s.error().kind));
    }
  }
  return Outcome::pass();
}

// --- from_json -------------------------------------------------------------

Outcome run_from_json(const Json& tc, const harness::Context& ctx) {
  std::string err;
  const Json doc = case_catalog(tc, ctx, &err);
  if (doc.is_undefined()) return Outcome::fail("cannot load catalog: " + err);
  const std::string version = case_version(tc, doc);
  const Json* expect_error = harness::find_expect_error(tc);
  const bool neutral_error = version.empty() && expect_error;
  if (!neutral_error && version != "1.0") return Outcome::unsupported("pre-v1.0 catalog");

  auto r = harness::load_catalog(doc, str(tc["catalogId"]), str(tc["protocolVersion"]));
  if (expect_error) {
    if (r.ok()) return harness::missing_error(*expect_error);
    return harness::check_error(r.error(), *expect_error);
  }
  if (!r.ok()) return Outcome::fail("from_json failed: " + r.error().message);
  const Catalog& cat = *r.value();
  const Json& ex = tc["expect"];

  if (ex["catalogId"].is_string() && cat.id() != ex["catalogId"].as_string()) {
    return Outcome::fail("catalogId " + cat.id() + ", expected " + ex["catalogId"].as_string());
  }
  if (ex["protocolVersion"].is_string() &&
      cat.protocol_version() != a2ui::normalize_protocol_version(ex["protocolVersion"].as_string())) {
    return Outcome::fail("protocolVersion " + cat.protocol_version());
  }
  if (ex["components"].is_container()) {
    const Json& c = ex["components"];
    std::vector<std::string> names;
    if (c.is_array()) {
      for (const auto& n : c.as_array()) names.push_back(str(n));
    } else {
      for (const auto& m : c.as_object()) names.push_back(m.first);
    }
    for (const auto& n : names) {
      if (!cat.find_component(n)) return Outcome::fail("missing component " + n);
    }
  }
  if (ex["functions"].is_container()) {
    const Json& f = ex["functions"];
    std::vector<std::string> names;
    if (f.is_array()) {
      for (const auto& n : f.as_array()) names.push_back(str(n));
    } else {
      for (const auto& m : f.as_object()) names.push_back(m.first);
    }
    for (const auto& n : names) {
      if (!cat.find_function(n)) return Outcome::fail("missing function " + n);
    }
  }
  for (const auto& m : ex["componentMetadata"].as_object()) {
    const a2ui::ComponentApi* api = cat.find_component(m.first);
    if (!api) return Outcome::fail("missing component " + m.first);
    const Json got = api->metadata.is_undefined() ? Json(nullptr) : api->metadata;
    auto mm = harness::json_mismatch(m.second, got);
    if (!mm.empty()) return Outcome::fail("metadata of " + m.first + ": " + mm);
  }
  if (const Json* theme = ex.find("theme")) {
    auto mm = compare_documents(*theme, cat.to_json()["theme"]);
    if (!mm.empty()) return Outcome::fail("theme: " + mm);
  }
  if (ex["selfContained"].as_bool()) {
    const std::string p = self_contained_problem(cat.validation_schema());
    if (!p.empty()) return Outcome::fail("not self-contained: " + p);
  }
  if (ex["matchesBuiltinCatalog"].as_bool()) {
    CatalogPtr builtin = builtin_basic_catalog();
    if (!builtin || builtin->id() != cat.id()) {
      return Outcome::unsupported("no built-in catalog " + cat.id());
    }
    if (builtin->protocol_version() != cat.protocol_version()) return Outcome::fail("protocolVersion mismatch");
    auto mm = harness::json_mismatch(component_shapes(*builtin), component_shapes(cat));
    if (!mm.empty()) return Outcome::fail("component mismatch: " + mm);
    mm = harness::json_mismatch(function_signatures(*builtin), function_signatures(cat));
    if (!mm.empty()) return Outcome::fail("function mismatch: " + mm);
  }
  for (const auto& comp : ex["validComponents"].as_array()) {
    Outcome o = check_component_sample(cat, comp, true);
    if (!o.passed()) return o;
  }
  for (const auto& comp : ex["invalidComponents"].as_array()) {
    Outcome o = check_component_sample(cat, comp, false);
    if (!o.passed()) return o;
  }
  return Outcome::pass();
}

// --- to_json / round_trip / validation_schema -------------------------------

Outcome run_to_json(const Json& tc, const harness::Context& ctx) {
  const std::string version = a2ui::normalize_protocol_version(str(tc["protocolVersion"]));
  if (version != "1.0") return Outcome::unsupported("pre-v1.0 catalog");
  CatalogPtr cat;
  if (str(tc["builtinCatalog"]) == "basic") {
    cat = builtin_basic_catalog();
  } else if (!tc["builtinCatalog"].is_undefined()) {
    return Outcome::unsupported("no built-in catalog " + str(tc["builtinCatalog"]));
  } else {
    std::string err;
    const Json doc = case_catalog(tc, ctx, &err);
    auto r = harness::load_catalog(doc, str(tc["catalogId"]), str(tc["protocolVersion"]));
    if (!r.ok()) return Outcome::fail("from_json failed: " + r.error().message);
    cat = r.value();
  }
  if (!cat) return Outcome::fail("cannot build catalog");
  std::string err;
  const Json expected = harness::load_json_file(ctx.repo_path(str(tc["expectFile"])), &err);
  if (expected.is_undefined()) return Outcome::fail("cannot load expectFile: " + err);
  auto mm = compare_documents(expected, cat->to_json());
  return mm.empty() ? Outcome::pass() : Outcome::fail("to_json differs: " + mm);
}

Outcome run_round_trip(const Json& tc, const harness::Context& ctx) {
  std::string err;
  const Json doc = case_catalog(tc, ctx, &err);
  if (doc.is_undefined()) return Outcome::fail("cannot load catalog: " + err);
  if (case_version(tc, doc) != "1.0") return Outcome::unsupported("pre-v1.0 catalog");
  auto r = harness::load_catalog(doc, str(tc["catalogId"]), str(tc["protocolVersion"]));
  if (!r.ok()) return Outcome::fail("from_json failed: " + r.error().message);
  const Json out = r.value()->to_json();
  auto mm = compare_documents(doc, out);
  if (!mm.empty()) return Outcome::fail("round trip differs: " + mm);
  auto again = harness::load_catalog(out, str(tc["catalogId"]), str(tc["protocolVersion"]));
  if (!again.ok()) return Outcome::fail("second from_json failed: " + again.error().message);
  mm = compare_documents(out, again.value()->to_json());
  return mm.empty() ? Outcome::pass() : Outcome::fail("second round trip differs: " + mm);
}

Outcome run_validation_schema(const Json& tc, const harness::Context& ctx) {
  std::string err;
  const Json doc = case_catalog(tc, ctx, &err);
  if (doc.is_undefined()) return Outcome::fail("cannot load catalog: " + err);
  if (case_version(tc, doc) != "1.0") return Outcome::unsupported("pre-v1.0 catalog");
  std::string id = str(tc["catalogId"]);
  auto r = harness::load_catalog(doc, id, str(tc["protocolVersion"]));
  if (const Json* ee = harness::find_expect_error(tc)) {
    if (r.ok()) return harness::missing_error(*ee);
    return harness::check_error(r.error(), *ee);
  }
  if (!r.ok()) return Outcome::fail("from_json failed: " + r.error().message);
  const Json expected = harness::load_json_file(ctx.repo_path(str(tc["expectFile"])), &err);
  if (expected.is_undefined()) return Outcome::fail("cannot load expectFile: " + err);
  auto mm = compare_documents(expected, r.value()->validation_schema());
  return mm.empty() ? Outcome::pass() : Outcome::fail("validation_schema differs: " + mm);
}

// --- derive ------------------------------------------------------------------

bool derive_entries(const Json& listed, std::vector<a2ui::DeriveEntry>* out) {
  for (const auto& item : listed.as_array()) {
    if (item.is_string()) {
      out->push_back({item.as_string(), Json()});
    } else if (item.is_object() && item.size() == 1) {
      const auto& m = item.as_object().front();
      out->push_back({m.first, m.second});
    } else {
      return false;
    }
  }
  return true;
}

Outcome run_derive(const Json& tc, const harness::Context& ctx) {
  std::string err;
  const Json doc = case_catalog(tc, ctx, &err);
  if (doc.is_undefined()) return Outcome::fail("cannot load catalog: " + err);
  if (case_version(tc, doc) != "1.0") return Outcome::unsupported("pre-v1.0 catalog");
  auto src = harness::load_catalog(doc);
  if (!src.ok()) return Outcome::fail("from_json failed: " + src.error().message);
  const Json& args = tc["args"];
  std::vector<a2ui::DeriveEntry> comps, fns;
  const bool has_c = args["components"].is_array();
  const bool has_f = args["functions"].is_array();
  if ((has_c && !derive_entries(args["components"], &comps)) ||
      (has_f && !derive_entries(args["functions"], &fns))) {
    return Outcome::fail("malformed derive args");
  }
  const std::string instructions = str(args["instructions"]);
  auto d = src.value()->derive(str(args["catalogId"]), has_c ? &comps : nullptr, has_f ? &fns : nullptr,
                               args["instructions"].is_string() ? &instructions : nullptr);
  if (const Json* ee = harness::find_expect_error(tc)) {
    if (d.ok()) return harness::missing_error(*ee);
    return harness::check_error(d.error(), *ee);
  }
  if (!d.ok()) return Outcome::fail("derive failed: " + d.error().message);
  const Catalog& cat = *d.value();
  const Json out = cat.to_json();
  const Json& ex = tc["expect"];
  if (ex["catalogId"].is_string() && cat.id() != ex["catalogId"].as_string()) {
    return Outcome::fail("catalogId " + cat.id());
  }
  if (ex["protocolVersion"].is_string() &&
      cat.protocol_version() != a2ui::normalize_protocol_version(ex["protocolVersion"].as_string())) {
    return Outcome::fail("protocolVersion " + cat.protocol_version());
  }
  for (const char* kind : {"components", "functions"}) {
    const Json& listed = ex[kind];
    if (listed.is_undefined()) continue;
    std::vector<std::string> want, got;
    if (listed.is_array()) {
      for (const auto& n : listed.as_array()) want.push_back(str(n));
    } else {
      for (const auto& m : listed.as_object()) want.push_back(m.first);
    }
    if (std::string(kind) == "components") {
      for (const auto& c : cat.components()) got.push_back(c.name);
    } else {
      for (const auto& f : cat.functions()) got.push_back(f.api.name);
    }
    std::sort(want.begin(), want.end());
    std::sort(got.begin(), got.end());
    if (want != got) return Outcome::fail(std::string(kind) + " mismatch");
    if (listed.is_object()) {
      for (const auto& m : listed.as_object()) {
        auto mm = compare_documents(m.second, out[kind][m.first]);
        if (!mm.empty()) return Outcome::fail(std::string(kind) + " '" + m.first + "' differs: " + mm);
      }
    }
  }
  if (const Json* theme = ex.find("theme")) {
    auto mm = compare_documents(*theme, out["theme"]);
    if (!mm.empty()) return Outcome::fail("theme: " + mm);
  }
  if (ex["instructions"].is_string() && str(out["instructions"]) != ex["instructions"].as_string()) {
    return Outcome::fail("instructions " + out["instructions"].dump());
  }
  for (const auto& k : ex["absentKeys"].as_array()) {
    if (out.contains(str(k))) return Outcome::fail("to_json carries '" + str(k) + "'");
  }
  return Outcome::pass();
}

// --- common types ------------------------------------------------------------

Outcome run_validate_common_type(const Json& tc, const harness::Context&) {
  const std::string version = a2ui::normalize_protocol_version(str(tc["protocolVersion"]));
  if (version != "1.0") return Outcome::unsupported("pre-v1.0 common types");
  const std::string def = str(tc["definition"]);
  int step_no = 0;
  for (const auto& step : tc["steps"].as_array()) {
    ++step_no;
    const std::string where = "step " + std::to_string(step_no) + " " + step["value"].dump() + ": ";
    a2ui::Status s = a2ui::validate_common_type(version, def, step["value"]);
    if (const Json* ee = harness::find_expect_error(step)) {
      if (s.ok()) return Outcome::fail(where + "expected an error");
      Outcome o = harness::check_error(s.error(), *ee);
      if (!o.passed()) return Outcome::fail(where + o.detail);
    } else if (!s.ok()) {
      return Outcome::fail(where + s.error().message);
    }
    // A valid value is kept as given, so it serializes back unchanged.
  }
  return Outcome::pass();
}

Outcome compare_spec_document(const Json& tc, const harness::Context& ctx, a2ui::spec::Document d) {
  const std::string version = a2ui::normalize_protocol_version(str(tc["protocolVersion"]));
  if (version != "1.0" || str(tc["protocolVersion"]).find("0.9.") != std::string::npos) {
    return Outcome::unsupported("pre-v1.0 schema");
  }
  std::string err;
  const Json expected = harness::load_json_file(ctx.repo_path(str(tc["expectFile"])), &err);
  if (expected.is_undefined()) return Outcome::fail("cannot load expectFile: " + err);
  auto mm = harness::json_mismatch(expected, *a2ui::spec::parsed(d));
  return mm.empty() ? Outcome::pass() : Outcome::fail(mm);
}

Outcome run_common_types_schema(const Json& tc, const harness::Context& ctx) {
  return compare_spec_document(tc, ctx, a2ui::spec::Document::CommonTypesV1_0);
}

Outcome run_agent_to_renderer_schema(const Json& tc, const harness::Context& ctx) {
  return compare_spec_document(tc, ctx, a2ui::spec::Document::AgentToRendererV1_0);
}

harness::ActionRegistrar from_json_registrar("from_json", &run_from_json);
harness::ActionRegistrar to_json_registrar("to_json", &run_to_json);
harness::ActionRegistrar round_trip_registrar("round_trip", &run_round_trip);
harness::ActionRegistrar validation_schema_registrar("validation_schema", &run_validation_schema);
harness::ActionRegistrar derive_registrar("derive", &run_derive);
harness::ActionRegistrar validate_common_type_registrar("validate_common_type", &run_validate_common_type);
harness::ActionRegistrar common_types_schema_registrar("common_types_schema", &run_common_types_schema);
harness::ActionRegistrar agent_to_renderer_schema_registrar("agent_to_renderer_schema",
                                                            &run_agent_to_renderer_schema);

}  // namespace

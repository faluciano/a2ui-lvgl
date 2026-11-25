// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Runners for the processing-layer actions: process_messages, validate,
// get_renderer_capabilities, get_renderer_data_model, select_catalog,
// evaluate_function, dispatch_action and measure_growth. Case semantics
// follow the Python reference harness
// (python/a2ui_core/tests/test_conformance.py): how catalogPaths, catalogs,
// catalog and createSurface catalog ids become loaded catalogs, how surface
// expectations are compared against resolved component properties, and how
// expectError (details, rendererError) is checked.
//
// Only protocol v1.0 is implemented; cases for earlier versions are
// reported as unsupported.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "a2ui/basic_catalog.h"
#include "a2ui/catalog.h"
#include "a2ui/coerce.h"
#include "a2ui/data_context.h"
#include "a2ui/locale.h"
#include "a2ui/message_processor.h"
#include "a2ui/node_resolver.h"
#include "a2ui/schema.h"
#include "a2ui/spec_documents.h"
#include "a2ui/surface_model.h"
#include "harness.h"

namespace {

using a2ui::Catalog;
using a2ui::CatalogPtr;
using a2ui::Error;
using a2ui::ErrorKind;
using a2ui::MessageProcessor;
using a2ui::SurfaceModel;
using a2ui::ValidationConfig;
using harness::Json;
using harness::Outcome;

std::string str(const Json& j) { return j.is_string() ? j.as_string() : std::string(); }

// --- Catalogs ---------------------------------------------------------------

const CatalogPtr& basic_v1() {
  static const CatalogPtr cat = [] {
    auto r = harness::load_basic_catalog();
    CatalogPtr c = r.ok() ? r.value() : a2ui::make_ref<Catalog>();
    a2ui::basic_catalog::bind_functions(*c);
    return c;
  }();
  return cat;
}

/** A copy of the basic catalog under another id, with or without its functions. */
CatalogPtr basic_alias(const std::string& id, bool with_functions) {
  static const std::vector<a2ui::DeriveEntry> kNone;
  auto r = basic_v1()->derive(id, nullptr, with_functions ? nullptr : &kNone);
  if (!r.ok()) return nullptr;
  CatalogPtr c = r.value();
  if (with_functions) a2ui::basic_catalog::bind_functions(*c);
  return c;
}

/** Normalized version of a case: protocolVersion, catalog.*, catalogs[], else the first message's version. */
std::string case_version(const Json& tc) {
  std::string v = str(tc["protocolVersion"]);
  if (v.empty()) v = str(tc["catalog"]["protocolVersion"]);
  if (v.empty()) v = str(tc["catalogSchema"]["protocolVersion"]);
  if (v.empty()) {
    for (const auto& c : tc["catalogs"].as_array()) {
      if (c["protocolVersion"].is_string()) {
        v = c["protocolVersion"].as_string();
        break;
      }
    }
  }
  if (v.empty()) return "1.0";
  return a2ui::normalize_protocol_version(v);
}

bool is_catalog_error_case(const Json& tc) {
  const Json* e = harness::find_expect_error(tc);
  return e && (str((*e)["category"]) == "CatalogError");
}

void collect_messages(const Json& node, std::vector<const Json*>* out, int depth = 0) {
  if (depth > 6) return;
  if (node.is_array()) {
    for (const auto& e : node.as_array()) collect_messages(e, out, depth + 1);
  } else if (node.is_object()) {
    if (node["messages"].is_array()) collect_messages(node["messages"], out, depth + 1);
    out->push_back(&node);
  }
}

struct CaseCatalogs {
  std::vector<CatalogPtr> list;
  std::string error;
};

CatalogPtr from_json_or_note(const Json& doc, const std::string& id, const std::string& version, std::string* error) {
  auto r = harness::load_catalog(doc, id, version);
  if (!r.ok()) {
    *error = r.error().message;
    return nullptr;
  }
  CatalogPtr c = r.value();
  if (c->is_v1()) a2ui::basic_catalog::bind_functions(*c);
  return c;
}

CatalogPtr inline_catalog(const Json& spec, const std::string& id, const std::string& version, bool forbid_unlisted,
                          bool capabilities, std::string* error) {
  const bool has_content = spec["components"].is_object() || spec.contains("theme") || spec.contains("functions");
  if (has_content) {
    Json doc = Json::object();
    if (spec.contains("$schema")) doc.set("$schema", spec["$schema"]);
    if (spec.contains("protocolVersion")) doc.set("protocolVersion", spec["protocolVersion"]);
    doc.set("catalogId", Json(id));
    if (forbid_unlisted && !spec.contains("functions") && !spec.contains("$defs")) {
      doc.set("$defs", Json::object({{"anyFunction", Json::object({{"not", Json::object()}})}}));
    }
    if (spec.contains("components")) doc.set("components", spec["components"]);
    if (spec.contains("theme")) doc.set("theme", spec["theme"]);
    if (spec.contains("functions")) doc.set("functions", spec["functions"]);
    if (spec.contains("$defs")) doc.set("$defs", spec["$defs"]);
    return from_json_or_note(doc, id, version, error);
  }
  if (capabilities || a2ui::normalize_protocol_version(version) != "1.0") {
    auto c = a2ui::make_ref<Catalog>();
    c->set_id(id);
    c->set_protocol_version(a2ui::normalize_protocol_version(version));
    return c;
  }
  return basic_alias(id, false);
}

/** Loads the catalog at a repository-relative path (as addCatalogPaths names them). */
CatalogPtr load_catalog_path(const std::string& path, const harness::Context& ctx, const std::string& version,
                             std::string* error) {
  Json doc = harness::load_json_file(ctx.repo_path(path), error);
  if (!doc.is_object()) return nullptr;
  std::string id = str(doc["catalogId"]);
  if (id.empty()) id = str(doc["id"]);
  if (id.empty()) id = "test-catalog";
  std::string v = str(doc["protocolVersion"]);
  if (v.empty()) v = path.find("v0_9") != std::string::npos ? "v0.9" : version;
  return from_json_or_note(doc, id, v, error);
}

/** Ids of the catalogs that the case's steps add with addCatalogPaths. */
std::set<std::string> added_catalog_ids(const Json& tc, const harness::Context& ctx) {
  std::set<std::string> ids;
  for (const auto& step : tc["steps"].as_array()) {
    for (const auto& p : step["addCatalogPaths"].as_array()) {
      std::string err;
      Json doc = harness::load_json_file(ctx.repo_path(str(p)), &err);
      std::string id = str(doc["catalogId"]);
      if (id.empty()) id = str(doc["id"]);
      if (!id.empty()) ids.insert(id);
    }
  }
  return ids;
}

CaseCatalogs catalogs_for_case(const Json& tc, const harness::Context& ctx) {
  CaseCatalogs out;
  const std::string version = case_version(tc);
  const std::string action = str(tc["action"]);
  const bool capabilities = action == "get_renderer_capabilities";
  std::map<std::string, CatalogPtr> by_id;
  std::vector<CatalogPtr> specified;
  auto add = [&](CatalogPtr c) {
    if (!c) return;
    by_id[c->id()] = c;
    specified.push_back(c);
  };

  if (const Json& spec = tc["catalog"]; spec.is_object()) {
    std::string id = str(tc["catalogId"]);
    if (id.empty()) id = str(spec["catalogId"]);
    if (spec["catalogSchema"].is_object()) {
      if (id.empty()) id = str(spec["catalogSchema"]["catalogId"]);
      if (id.empty()) id = "catalog-" + str(tc["name"]);
      add(from_json_or_note(spec["catalogSchema"], id, version, &out.error));
    } else if (spec.contains("components") || spec.contains("catalogId")) {
      if (id.empty()) id = "catalog-" + str(tc["name"]);
      add(inline_catalog(spec, id, version, true, capabilities, &out.error));
    }
  }
  for (const auto& item : tc["catalogs"].as_array()) {
    if (item["catalogSchema"].is_object()) {
      std::string id = str(item["catalogSchema"]["catalogId"]);
      if (id.empty()) id = str(item["catalogId"]);
      std::string v = str(item["protocolVersion"]);
      if (v.empty()) v = str(item["catalogSchema"]["protocolVersion"]);
      if (v.empty()) v = "v0.9";
      if (!id.empty()) add(from_json_or_note(item["catalogSchema"], id, v, &out.error));
    } else if (item["catalogId"].is_string()) {
      std::string v = str(item["protocolVersion"]);
      if (v.empty()) v = version;
      add(inline_catalog(item, item["catalogId"].as_string(), v, false, capabilities, &out.error));
    }
  }
  for (const auto& p : tc["catalogPaths"].as_array()) {
    std::string err;
    Json doc = harness::load_json_file(ctx.repo_path(str(p)), &err);
    if (!doc.is_object()) {
      out.error = err;
      continue;
    }
    std::string id = str(doc["catalogId"]);
    if (id.empty()) id = str(doc["id"]);
    if (id.empty()) id = "test-catalog";
    if (id == a2ui::basic_catalog::kCatalogIdV1 || str(p).find("basic/v1/catalog.json") != std::string::npos) {
      add(basic_v1());
      continue;
    }
    std::string v = str(doc["protocolVersion"]);
    if (v.empty()) v = str(p).find("v0_9") != std::string::npos ? "v0.9" : version;
    add(from_json_or_note(doc, id, v, &out.error));
    if (id != "test-catalog") {
      CatalogPtr alias = from_json_or_note(doc, "test-catalog", v, &out.error);
      if (alias) by_id["test-catalog"] = alias, specified.push_back(alias);
    }
  }

  if (capabilities && !specified.empty()) {
    out.list = specified;
    return out;
  }
  // The basic catalog, and "basic" as a copy of it.
  std::vector<CatalogPtr> extra;
  auto ensure = [&](const std::string& id, CatalogPtr c) {
    if (by_id.count(id) || !c) return;
    by_id[id] = c;
    extra.push_back(c);
  };
  if (version == "1.0") {
    ensure(a2ui::basic_catalog::kCatalogIdV1, basic_v1());
    ensure("basic", basic_alias("basic", true));
  }
  if (!is_catalog_error_case(tc)) {
    // A catalog a step adds gets no stand-in under its id.
    const std::set<std::string> added = added_catalog_ids(tc, ctx);
    std::vector<const Json*> msgs;
    if (tc.contains("messages")) collect_messages(tc["messages"], &msgs);
    if (tc.contains("payload")) collect_messages(tc["payload"], &msgs);
    for (const auto& step : tc["steps"].as_array()) {
      collect_messages(step["messages"], &msgs);
      collect_messages(step["payload"], &msgs);
    }
    for (const Json* m : msgs) {
      const Json& cs = (*m)["createSurface"];
      if (!cs["catalogId"].is_string()) continue;
      const std::string id = cs["catalogId"].as_string();
      const std::string v = (*m)["version"].is_string() ? a2ui::normalize_protocol_version((*m)["version"].as_string())
                                                        : version;
      if (v == "1.0" && !added.count(id)) ensure(id, basic_alias(id, false));
    }
  }
  out.list = specified;
  // The basic catalog comes right after the specified ones.
  for (const auto& c : extra) {
    if (c == basic_v1()) out.list.push_back(c);
  }
  for (const auto& c : extra) {
    if (c != basic_v1()) out.list.push_back(c);
  }
  return out;
}

// --- Errors -----------------------------------------------------------------

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string norm_quotes(std::string s) {
  s = lower(std::move(s));
  for (char& c : s) {
    if (c == '"') c = '\'';
  }
  return s;
}

bool message_matches(const std::string& expected, const std::string& actual) {
  const std::string e = norm_quotes(expected), a = norm_quotes(actual);
  if (a.find(e) != std::string::npos) return true;
  auto has = [&](const char* t) { return a.find(t) != std::string::npos; };
  if (e.find("not of type") != std::string::npos && (has("input should be") || has("type_mismatch") || has("must be"))) return true;
  if (e.find("is a required property") != std::string::npos && (has("field required") || has("missing"))) return true;
  if (e.find("orphan") != std::string::npos && (has("orphan") || has("not reachable"))) return true;
  if (e.find("circular") != std::string::npos && (has("circular") || has("self-reference"))) return true;
  return harness::regex_search(expected, actual);
}

bool category_ok(const Error& e, const std::string& category) {
  if (category.empty()) return true;
  if (category == "IntegrityError") return e.is(ErrorKind::Integrity) || e.is(ErrorKind::Recursion);
  if (category == "ParseError") return e.is(ErrorKind::Parse) || e.is(ErrorKind::Expression);
  static const std::map<std::string, ErrorKind> kinds = {
      {"ValidationError", ErrorKind::Validation}, {"RecursionError", ErrorKind::Recursion},
      {"CatalogError", ErrorKind::Catalog},       {"DataError", ErrorKind::Data},
      {"ExpressionError", ErrorKind::Expression}, {"StateError", ErrorKind::State},
      {"Error", ErrorKind::Generic}};
  auto it = kinds.find(category);
  return it != kinds.end() && e.is(it->second);
}

std::string describe(const Error& e) {
  return std::string(a2ui::error_kind_name(e.kind)) + " [" + e.code + "] path='" + e.path + "' surface='" +
         e.surface_id + "': " + e.message + " details=" + e.details.dump();
}

/** A dotted validation path ("messages.0.createSurface.x[1]") as a pointer relative to the action. */
std::string to_pointer(const std::string& p) {
  if (p.empty()) return p;
  std::string norm;
  for (char c : p) {
    if (c == '[') {
      norm += '.';
    } else if (c != ']') {
      norm += c;
    }
  }
  if (!norm.empty() && norm[0] == '/') return norm;
  std::vector<std::string> segs;
  size_t i = 0;
  while (i <= norm.size()) {
    size_t j = norm.find('.', i);
    if (j == std::string::npos) j = norm.size();
    if (j > i) segs.push_back(norm.substr(i, j - i));
    i = j + 1;
  }
  size_t k = 0;
  if (k < segs.size() && segs[k] == "messages") {
    ++k;
    if (k < segs.size() && segs[k].find_first_not_of("0123456789") == std::string::npos) ++k;
  }
  static const char* const kActions[] = {"createSurface", "updateComponents", "updateDataModel", "deleteSurface",
                                         "callRendererFunction", "agentFunctionResponse"};
  if (k < segs.size()) {
    for (const char* a : kActions) {
      if (segs[k] == a) {
        ++k;
        break;
      }
    }
  }
  std::string out;
  for (; k < segs.size(); ++k) out += "/" + segs[k];
  return out;
}

/** The Python harness's _detail_path_matches, with our pointer-shaped detail paths. */
bool detail_path_matches(const std::string& actual, const std::string& expected) {
  const std::string want = to_pointer(expected);
  if (actual == expected || actual == want) return true;
  if (actual.rfind("/components/", 0) == 0) {
    const size_t cut = actual.find('/', 12);
    const std::string rel = cut == std::string::npos ? std::string() : actual.substr(cut);
    if (!rel.empty() && rel == want) return true;
  }
  return false;
}

/** Checks an error against expectError the way the Python harness does. */
Outcome check_full_error(const Error& error, const Json& expect, const std::vector<Json>& renderer_errors) {
  if (!expect.is_object()) return Outcome::pass();
  if (!category_ok(error, str(expect["category"]))) {
    return Outcome::fail("expected " + str(expect["category"]) + ", got " + describe(error));
  }
  const Json& details = error.details;
  if (expect["code"].is_string()) {
    const std::string code = expect["code"].as_string();
    bool ok = error.code == code || error.message.find(code) != std::string::npos;
    for (const auto& d : details.as_array()) ok = ok || str(d["code"]) == code;
    if (!ok) return Outcome::fail("expected code " + code + ", got " + describe(error));
  }
  if (expect["message"].is_string()) {
    const std::string m = expect["message"].as_string();
    std::string all = error.message;
    for (const auto& d : details.as_array()) all += " " + str(d["message"]);
    if (!message_matches(m, all)) return Outcome::fail("message /" + m + "/ did not match " + describe(error));
  }
  if (expect["path"].is_string()) {
    const std::string p = expect["path"].as_string();
    bool ok = error.path == p;
    for (const auto& d : details.as_array()) ok = ok || str(d["path"]) == p;
    if (!ok) return Outcome::fail("expected path " + p + ", got " + describe(error));
  }
  if (expect["surfaceId"].is_string()) {
    const std::string s = expect["surfaceId"].as_string();
    bool ok = error.surface_id == s;
    for (const auto& d : details.as_array()) ok = ok || str(d["surfaceId"]) == s;
    if (!ok) return Outcome::fail("expected surfaceId '" + s + "', got " + describe(error));
  }
  if (expect["functionCallId"].is_string() && error.function_call_id != expect["functionCallId"].as_string()) {
    return Outcome::fail("expected functionCallId " + expect["functionCallId"].as_string() + ", got " + describe(error));
  }
  for (const auto& ed : expect["details"].as_array()) {
    bool found = false;
    for (const auto& d : details.as_array()) {
      if (ed["path"].is_string() && !detail_path_matches(str(d["path"]), ed["path"].as_string())) continue;
      if (ed["code"].is_string() && str(d["code"]) != ed["code"].as_string() &&
          !(ed["code"].as_string() == "invalid_value" &&
            (str(d["code"]) == "type_mismatch" || str(d["code"]) == "unrecognized_component"))) {
        continue;
      }
      if (ed["message"].is_string() && lower(str(d["message"])).find(lower(ed["message"].as_string())) == std::string::npos) {
        continue;
      }
      found = true;
      break;
    }
    if (!found) return Outcome::fail("expected detail " + ed.dump() + " in " + describe(error));
  }
  if (const Json& re = expect["rendererError"]; re.is_object()) {
    if (renderer_errors.empty()) return Outcome::fail("no rendererError was emitted; error was " + describe(error));
    const Json& msg = renderer_errors.back();
    if (re["version"].is_string() && str(msg["version"]) != re["version"].as_string()) {
      return Outcome::fail("rendererError.version " + msg.dump());
    }
    const Json& exp = re["error"].is_object() ? re["error"] : re;
    const Json& got = msg["error"];
    for (const char* k : {"code", "surfaceId", "functionCallId"}) {
      if (exp.contains(k) && got[k] != exp[k]) {
        return Outcome::fail(std::string("rendererError.") + k + " expected " + exp[k].dump() + ", got " + msg.dump());
      }
    }
    if (exp.contains("path") && got["path"] != exp["path"]) {
      // A missing property is reported at the property here and at its
      // object in other SDKs; the Python harness accepts either.
      bool ok = false;
      for (const auto& d : details.as_array()) {
        if (str(d["code"]) != "missing_field") continue;
        const std::string m = str(d["message"]);
        const size_t q = m.find('\''), q2 = q == std::string::npos ? q : m.find('\'', q + 1);
        if (q2 != std::string::npos && Json(str(exp["path"]) + "/" + m.substr(q + 1, q2 - q - 1)) == got["path"]) ok = true;
      }
      if (!ok) return Outcome::fail("rendererError.path expected " + exp["path"].dump() + ", got " + msg.dump());
    }
    if (exp.contains("functionCallId") && !exp.contains("surfaceId") && got.contains("surfaceId")) {
      return Outcome::fail("rendererError has a surfaceId for a function call error: " + msg.dump());
    }
    if (exp["message"].is_string() && !message_matches(exp["message"].as_string(), str(got["message"]))) {
      return Outcome::fail("rendererError.message expected '" + exp["message"].as_string() + "' in " + msg.dump());
    }
  }
  return Outcome::pass();
}

// --- Resolution (the Python harness's _resolve_surface_components) -----------
//
// Surface expectations name components by id, including orphans and
// definitions resolved at "/", plus template instances as `<id>_<i>` and
// `<id>-[<scope>]`. Each instance is resolved by the core's one-shot binder
// (a2ui::resolve_component_snapshot, the per-component resolution the
// NodeResolver uses; checks evaluated by a2ui::evaluate_checks); this
// walk only decides which instances the expectations can address.

struct Node {
  std::string type;
  Json props;
};

class Resolver {
 public:
  explicit Resolver(SurfaceModel& s) : surface_(s) {}

  std::map<std::string, Node> run() {
    std::vector<std::string> ids;
    for (const auto& m : surface_.components_model().components()) ids.push_back(m.first);
    for (const auto& id : ids) resolve_one(id, "/", id, 0);
    return std::move(out_);
  }

 private:
  void resolve_one(const std::string& id, const std::string& data_path, const std::string& key, int depth) {
    if (depth > 60 || out_.count(key) || pending_.count(key)) return;
    const a2ui::ComponentModel* comp = surface_.components_model().get(id);
    if (!comp) return;
    pending_.insert(key);
    const std::optional<long> index = a2ui::scope_index_of(data_path);
    Node node;
    node.type = comp->type();
    node.props = a2ui::resolve_component_snapshot(surface_, *comp, data_path);
    // Template children, one instance per item.
    for (const auto& m : comp->properties().as_object()) {
      const Json& v = m.second;
      if (!v.is_object() || !v["componentId"].is_string()) continue;
      const Json& raw = v["@path"].is_string() ? v["@path"] : v["path"];
      if (!raw.is_string()) continue;
      const std::string tpl_path = a2ui::path::resolve(raw.as_string(), data_path);
      const Json arr = surface_.data_model().value_at(tpl_path);
      if (!arr.is_array()) continue;
      std::string base = tpl_path;
      while (base.size() > 1 && base.back() == '/') base.pop_back();
      if (base == "/") base.clear();
      const std::string tpl = v["componentId"].as_string();
      for (size_t i = 0; i < arr.size(); ++i) {
        const std::string scoped = base + "/" + std::to_string(i);
        resolve_one(tpl, scoped, tpl + "_" + std::to_string(i), depth + 1);
        resolve_one(tpl, scoped, tpl + "-[" + scoped + "]", depth + 1);
      }
    }
    if (index) {
      for (const auto& child : surface_.components_model().get_child_ids(id)) {
        resolve_one(child, data_path, child + "_" + std::to_string(*index), depth + 1);
        resolve_one(child, data_path, child + "-[" + data_path + "]", depth + 1);
      }
    }
    pending_.erase(key);
    out_[key] = std::move(node);
  }

  SurfaceModel& surface_;
  std::map<std::string, Node> out_;
  std::set<std::string> pending_;
};

bool prop_matches(const Json& actual, const Json& expected) {
  if (actual == expected) return true;
  if (actual.is_number() && expected.is_string()) {
    const double n = actual.as_number();
    return std::floor(n) == n && a2ui::number_to_string(n) == expected.as_string();
  }
  return false;
}

std::string check_surfaces(MessageProcessor& p, const Json& expect) {
  for (const auto& m : expect["surfaces"].as_object()) {
    const std::string& sid = m.first;
    const Json& se = m.second;
    SurfaceModel* s = p.model().get_surface(sid);
    if (se["exists"].is_bool() && !se["exists"].as_bool()) {
      if (s) return "surface " + sid + " exists";
      continue;
    }
    if (!s) return "surface " + sid + " missing";
    if (se.contains("theme") && !(se["theme"].is_object() && se["theme"].empty())) {
      return "surface theme is not supported on v1.0 surfaces";
    }
    if (se.contains("metadata")) {
      const Json want = se["metadata"].is_null() ? Json() : se["metadata"];
      if (!(s->metadata() == want) && !(want.is_undefined() && s->metadata().is_undefined())) {
        return "surface metadata " + s->metadata().dump() + " != " + se["metadata"].dump();
      }
    }
    if (se.contains("sendDataModel") && Json(s->send_data_model()) != se["sendDataModel"]) {
      return "sendDataModel mismatch";
    }
    if (se.contains("dataModel")) {
      // Sparse array holes read as null, as in the other SDKs.
      const Json root = Json::parse(s->data_model().root().dump(), 256).value;
      if (!(se["dataModel"] == root)) {
        return "dataModel: " + harness::json_mismatch(se["dataModel"], root) + " got " + root.dump();
      }
    }
    std::map<std::string, Node> nodes;
    bool resolved = false;
    auto ensure_nodes = [&]() {
      if (!resolved) nodes = Resolver(*s).run();
      resolved = true;
    };
    if (se.contains("components")) {
      ensure_nodes();
      const Json& comps = se["components"];
      std::vector<std::pair<std::string, const Json*>> items;
      if (comps.is_object()) {
        for (const auto& c : comps.as_object()) items.emplace_back(c.first, &c.second);
      } else {
        for (const auto& c : comps.as_array()) items.emplace_back(str(c["id"]), &c);
      }
      bool all_present = true;
      for (const auto& it : items) {
        const a2ui::ComponentModel* comp = s->components_model().get(it.first);
        auto nit = nodes.find(it.first);
        if (!comp && nit == nodes.end()) return "component " + it.first + " missing from " + sid;
        all_present = all_present && comp;
        const Json& ce = *it.second;
        if (!ce.is_object()) continue;
        const std::string type = comp ? comp->type() : nit->second.type;
        if (ce.contains("component") && Json(type) != ce["component"]) {
          return "component " + it.first + " type " + type + " != " + ce["component"].dump();
        }
        if (ce.contains("@metadata")) {
          if (!comp) return "component " + it.first + " has no model";
          const Json want = ce["@metadata"].is_null() ? Json() : ce["@metadata"];
          if (!(comp->metadata() == want) && !(want.is_undefined() && comp->metadata().is_undefined())) {
            return "component " + it.first + " @metadata " + comp->metadata().dump() + " != " + ce["@metadata"].dump();
          }
        }
        if (nit == nodes.end()) continue;
        for (const auto& kv : ce.as_object()) {
          if (kv.first == "id" || kv.first == "component" || kv.first == "@metadata") continue;
          const Json& got = nit->second.props[kv.first];
          if (!prop_matches(got, kv.second)) {
            return "component " + it.first + " property " + kv.first + ": got " + got.dump() + ", expected " +
                   kv.second.dump();
          }
        }
      }
      if (comps.is_array() && all_present && s->components_model().size() != items.size()) {
        return "surface " + sid + " has " + std::to_string(s->components_model().size()) + " components, expected " +
               std::to_string(items.size());
      }
    }
    for (const char* field : {"validationResult", "validationResults", "isValid", "validationErrors"}) {
      if (!se.contains(field)) continue;
      ensure_nodes();
      for (const auto& c : se[field].as_object()) {
        auto nit = nodes.find(c.first);
        if (nit == nodes.end()) return std::string("node ") + c.first + " missing";
        const Json& got = nit->second.props[field];
        if (!(got == c.second)) {
          return std::string(field) + " of " + c.first + ": got " + got.dump() + ", expected " + c.second.dump();
        }
      }
    }
  }
  return "";
}

// --- validate / process_messages -------------------------------------------

std::optional<ValidationConfig> config_from_flags(const Json& flags) {
  ValidationConfig c;
  c.allow_orphan_components = flags["allowOrphanComponents"].as_bool();
  c.allow_dangling_references = flags["allowDanglingReferences"].as_bool();
  c.allow_missing_root = flags["allowMissingRoot"].as_bool();
  c.allow_unknown_elements = flags["allowUnknownElements"].as_bool();
  return c;
}

const Json& step_messages(const Json& step) {
  if (step.contains("messages")) return step["messages"];
  if (step.contains("payload")) return step["payload"];
  return step["message"];
}

bool is_pre_v1(const Json& v) {
  if (!v.is_string()) return false;
  const std::string n = a2ui::normalize_protocol_version(v.as_string());
  return n == "0.9" || n == "0.8";
}

/**
 * Cases whose protocol version, or one of whose messages, is before v1.0.
 * A v1.0 case may still name pre-1.0 catalogs (to check that mixing them
 * fails) or ask for a pre-1.0 data model view; those run.
 */
Outcome unsupported_for(const Json& tc) {
  if (is_pre_v1(tc["protocolVersion"]) || is_pre_v1(tc["catalog"]["protocolVersion"])) {
    return Outcome::unsupported("targets a protocol version before v1.0");
  }
  // v0.8 messages carry no version header; their actions give them away.
  std::vector<const Json*> msgs;
  collect_messages(tc["messages"], &msgs);
  collect_messages(tc["payload"], &msgs);
  for (const auto& step : tc["steps"].as_array()) {
    collect_messages(step["messages"], &msgs);
    collect_messages(step["payload"], &msgs);
  }
  for (const Json* m : msgs) {
    if (is_pre_v1((*m)["version"])) return Outcome::unsupported("a message targets a protocol version before v1.0");
    if (m->contains("version")) continue;
    for (const char* legacy : {"beginRendering", "surfaceUpdate", "dataModelUpdate"}) {
      if (m->contains(legacy)) return Outcome::unsupported("v0.8 message (no version header)");
    }
  }
  return Outcome::pass();
}

struct Runner {
  std::unique_ptr<MessageProcessor> processor;
  std::vector<Json> renderer_errors;
};

Outcome make_runner(const Json& tc, const harness::Context& ctx, std::optional<ValidationConfig> config,
                    Runner* r) {
  CaseCatalogs cats = catalogs_for_case(tc, ctx);
  if (!cats.error.empty() && !is_catalog_error_case(tc)) {
    return Outcome::fail("catalog load failed: " + cats.error);
  }
  a2ui::MessageProcessorOptions opts;
  opts.validation_config = config;
  std::vector<Json>* sink = &r->renderer_errors;
  opts.outbound_listener = [sink](const Json& m) {
    if (m.contains("error")) sink->push_back(m);
  };
  r->processor = std::make_unique<MessageProcessor>(cats.list, std::move(opts));
  return Outcome::pass();
}

/** Processes messages, then resolves every surface; the first error either reports is raised. */
a2ui::Status process_and_resolve(MessageProcessor& p, const Json& messages) {
  std::vector<Error> errors;
  std::vector<a2ui::Subscription> subs;
  a2ui::Status s = p.process_messages(messages);
  if (!s.ok()) return s;
  for (const auto& m : p.model().surfaces()) {
    SurfaceModel* surface = m.second.get();
    subs.push_back(surface->on_error().subscribe([&errors, surface](const Json& e) {
      Error err = a2ui::make_error(ErrorKind::Validation, str(e["message"]), str(e["code"]));
      err.path = str(e["path"]);
      err.surface_id = surface->id();
      errors.push_back(std::move(err));
    }));
    (void)Resolver(*surface).run();
  }
  if (!errors.empty()) return errors.front();
  return a2ui::ok_status();
}

HARNESS_ACTION("validate", run_validate) {
  if (Outcome u = unsupported_for(test_case); !u.passed()) return u;
  const bool relaxed = test_case["strictMode"].is_bool() && !test_case["strictMode"].as_bool();
  Runner r;
  if (Outcome o = make_runner(test_case, ctx, relaxed ? ValidationConfig::relaxed() : ValidationConfig::strict(), &r);
      !o.passed()) {
    return o;
  }
  const bool has_steps = test_case.contains("steps");
  std::vector<const Json*> steps;
  if (has_steps) {
    for (const auto& s : test_case["steps"].as_array()) steps.push_back(&s);
  } else {
    steps.push_back(&test_case);
  }
  for (size_t i = 0; i < steps.size(); ++i) {
    const Json& step = *steps[i];
    const bool last = i + 1 == steps.size();
    const Json* expect_error = harness::find_expect_error(step);
    if (!expect_error && last && has_steps) expect_error = harness::find_expect_error(test_case);
    if (expect_error) {
      r.renderer_errors.clear();
      a2ui::Status s = process_and_resolve(*r.processor, step_messages(step));
      if (s.ok()) return Outcome::fail("step " + std::to_string(i) + ": " + harness::missing_error(*expect_error).detail);
      Outcome o = check_full_error(s.error(), *expect_error, r.renderer_errors);
      if (!o.passed()) return Outcome::fail("step " + std::to_string(i) + ": " + o.detail);
    } else {
      a2ui::Status s = r.processor->process_messages(step_messages(step));
      if (!s.ok()) return Outcome::fail("step " + std::to_string(i) + " failed: " + describe(s.error()));
      const Json& expected = step.contains("expect") ? step["expect"] : (last ? test_case["expect"] : Json());
      if (expected.is_object()) {
        std::string mm = check_surfaces(*r.processor, expected);
        if (!mm.empty()) return Outcome::fail("step " + std::to_string(i) + ": " + mm);
      }
    }
  }
  return Outcome::pass();
}

HARNESS_ACTION("process_messages", run_process_messages) {
  if (Outcome u = unsupported_for(test_case); !u.passed()) return u;
  std::optional<ValidationConfig> config;
  if (test_case.contains("validationConfig")) {
    config = config_from_flags(test_case["validationConfig"]);
  } else if (test_case["strictMode"].as_bool() || test_case["strict_mode"].as_bool() ||
             test_case["options"]["strict_mode"].as_bool()) {
    config = ValidationConfig::strict();
  }
  Runner r;
  if (Outcome o = make_runner(test_case, ctx, config, &r); !o.passed()) return o;

  auto run_step = [&](const Json& messages, const Json* expect_error, const Json& expected,
                      const a2ui::ExecutionContext& ec) -> Outcome {
    if (expect_error) {
      r.renderer_errors.clear();
      a2ui::Status s = r.processor->process_messages(messages, ec);
      if (s.ok()) return harness::missing_error(*expect_error);
      return check_full_error(s.error(), *expect_error, r.renderer_errors);
    }
    a2ui::Status s = r.processor->process_messages(messages, ec);
    if (!s.ok()) return Outcome::fail("processing failed: " + describe(s.error()));
    if (expected.is_object()) {
      std::string mm = check_surfaces(*r.processor, expected);
      if (!mm.empty()) return Outcome::fail(mm);
    }
    return Outcome::pass();
  };

  if (test_case.contains("messages") || test_case.contains("payload")) {
    const Json& msgs = test_case.contains("messages") ? test_case["messages"] : test_case["payload"];
    return run_step(msgs, harness::find_expect_error(test_case), test_case["expect"], {});
  }
  const auto& steps = test_case["steps"].as_array();
  for (size_t i = 0; i < steps.size(); ++i) {
    const Json& step = steps[i];
    const bool last = i + 1 == steps.size();
    const Json* expect_error = harness::find_expect_error(step);
    if (!expect_error && last) expect_error = harness::find_expect_error(test_case);
    const Json& expected = step.contains("expect") ? step["expect"] : (last ? test_case["expect"] : Json());
    a2ui::ExecutionContext ec;
    if (step.contains("validationConfig")) ec.validation_config = config_from_flags(step["validationConfig"]);
    // addCatalogPaths: each catalog goes to add_catalog before the step's messages.
    bool add_failed = false;
    for (const auto& p : step["addCatalogPaths"].as_array()) {
      std::string err;
      CatalogPtr c = load_catalog_path(str(p), ctx, case_version(test_case), &err);
      if (!c) return Outcome::fail("step " + std::to_string(i) + ": catalog load failed: " + err);
      a2ui::Status s = r.processor->add_catalog(c);
      if (s.ok()) continue;
      if (!expect_error) return Outcome::fail("step " + std::to_string(i) + ": addCatalog failed: " + describe(s.error()));
      Outcome o = check_full_error(s.error(), *expect_error, r.renderer_errors);
      if (!o.passed()) return Outcome::fail("step " + std::to_string(i) + ": " + o.detail);
      add_failed = true;
      break;
    }
    if (add_failed) continue;
    if (!step.contains("messages") && !step.contains("payload") && !step.contains("message")) {
      if (expect_error) return Outcome::fail("step " + std::to_string(i) + ": " + harness::missing_error(*expect_error).detail);
      continue;
    }
    Outcome o = run_step(step_messages(step), expect_error, expected, ec);
    if (!o.passed()) return Outcome::fail("step " + std::to_string(i) + ": " + o.detail);
  }
  return Outcome::pass();
}

// --- capabilities / data model ----------------------------------------------

HARNESS_ACTION("get_renderer_capabilities", run_capabilities) {
  if (Outcome u = unsupported_for(test_case); !u.passed()) return u;
  Runner r;
  if (Outcome o = make_runner(test_case, ctx, std::nullopt, &r); !o.passed()) return o;
  a2ui::CapabilitiesOptions opts;
  const Json& args = test_case["args"];
  bool pre_v1 = is_pre_v1(args["version"]);
  for (const auto& v : args["versions"].as_array()) pre_v1 = pre_v1 || is_pre_v1(v);
  if (pre_v1) return Outcome::unsupported("capabilities for a protocol version before v1.0");
  if (args["versions"].is_array()) {
    for (const auto& v : args["versions"].as_array()) opts.versions.push_back(str(v));
  } else if (args["version"].is_string()) {
    opts.versions.push_back(args["version"].as_string());
  } else {
    opts.versions.push_back(case_version(test_case));
  }
  opts.include_inline_catalogs = args["includeInlineCatalogs"].as_bool();
  auto caps = r.processor->get_renderer_capabilities(opts);
  if (!caps.ok()) return Outcome::fail(describe(caps.error()));
  if (!(caps.value() == test_case["expect"])) {
    return Outcome::fail("got " + caps.value().dump() + ", " + harness::json_mismatch(test_case["expect"], caps.value()));
  }
  return Outcome::pass();
}

HARNESS_ACTION("get_renderer_data_model", run_data_model) {
  if (Outcome u = unsupported_for(test_case); !u.passed()) return u;
  Runner r;
  if (Outcome o = make_runner(test_case, ctx, std::nullopt, &r); !o.passed()) return o;
  std::vector<const Json*> batches;
  if (test_case.contains("messages")) batches.push_back(&test_case["messages"]);
  for (const auto& s : test_case["steps"].as_array()) batches.push_back(&step_messages(s));
  for (const Json* b : batches) {
    a2ui::Status s = r.processor->process_messages(*b);
    if (!s.ok()) return Outcome::fail("processing failed: " + describe(s.error()));
  }
  auto dm = r.processor->get_renderer_data_model(str(test_case["args"]["version"]));
  if (const Json* ee = harness::find_expect_error(test_case)) {
    if (dm.ok()) return harness::missing_error(*ee);
    return check_full_error(dm.error(), *ee, {});
  }
  if (!dm.ok()) return Outcome::fail(describe(dm.error()));
  const Json& want = test_case["expect"];
  if (!(dm.value() == want) && !(want.is_null() && dm.value().is_nullish())) {
    return Outcome::fail("got " + dm.value().dump() + ", expected " + want.dump());
  }
  return Outcome::pass();
}

// --- select_catalog -----------------------------------------------------------

HARNESS_ACTION("select_catalog", run_select_catalog) {
  if (Outcome u = unsupported_for(test_case); !u.passed()) return u;
  const Json& args = test_case["args"];
  const Json& sa = args["surface"];
  const std::string sid = sa["id"].is_string() ? sa["id"].as_string() : "main_surface";
  const std::string default_id = sa["defaultCatalogId"].is_string() ? sa["defaultCatalogId"].as_string() : "basic";
  a2ui::CatalogMap cats;
  auto stub = [](const std::string& id, const std::string& v) {
    auto c = a2ui::make_ref<Catalog>();
    c->set_id(id);
    c->set_protocol_version(a2ui::normalize_protocol_version(v));
    return c;
  };
  if (args["catalogs"].is_object()) {
    for (const auto& m : args["catalogs"].as_object()) {
      cats[m.first] = stub(m.first, m.second["protocolVersion"].is_string() ? m.second["protocolVersion"].as_string() : "1.0");
    }
  } else if (sa["supportedCatalogIds"].is_array()) {
    for (const auto& id : sa["supportedCatalogIds"].as_array()) cats[str(id)] = stub(str(id), "1.0");
  } else {
    cats[default_id] = stub(default_id, "1.0");
  }
  CatalogPtr def = cats.count(default_id) ? cats[default_id] : stub(default_id, "1.0");
  a2ui::SurfaceOptions so;
  so.protocol_version = "1.0";
  SurfaceModel surface(sid, def, cats, so);

  auto select = [&](bool check_versions) -> a2ui::Result<std::string> {
    if (check_versions) {
      for (const auto& c : cats) {
        if (c.second->protocol_version() != def->protocol_version()) {
          return a2ui::make_error(ErrorKind::Catalog,
                                  "Protocol version mismatch: cannot mix catalog '" + c.first + "' (" +
                                      (c.second->protocol_version() == "1.0" ? "1.0" : "v" + c.second->protocol_version()) +
                                      ") with surface version " + def->protocol_version() + ".");
        }
      }
    }
    std::string last;
    if (args["components"].is_object()) {
      for (const auto& m : args["components"].as_object()) {
        const Json& cid = m.second["catalogId"];
        const std::string* named = cid.is_string() && !cid.as_string().empty() ? &cid.as_string() : nullptr;
        if (named && !cats.count(*named)) {
          return a2ui::make_error(ErrorKind::Catalog,
                                  "Catalog '" + *named + "' is not supported by surface '" + sid + "'.");
        }
        auto r = surface.resolve_catalog(named, "Component '" + m.first + "'");
        if (!r.ok()) return r.take_error();
        last = r.value()->id();
        const std::string type = m.second["component"].is_string() ? m.second["component"].as_string() : "Box";
        (void)surface.components_model().add_component(
            std::make_unique<a2ui::ComponentModel>(m.first, type, r.value(), Json::object()));
      }
    } else if (args["functionCall"].is_object()) {
      const Json& fc = args["functionCall"];
      const std::string* named = fc["catalogId"].is_string() ? &fc["catalogId"].as_string() : nullptr;
      auto r = surface.resolve_catalog(named, "Function call");
      if (!r.ok()) return r.take_error();
      last = r.value()->id();
    }
    return last;
  };
  if (const Json* ee = harness::find_expect_error(test_case)) {
    auto r = select(true);
    if (r.ok()) return harness::missing_error(*ee);
    return check_full_error(r.error(), *ee, {});
  }
  auto r = select(false);
  if (!r.ok()) return Outcome::fail(describe(r.error()));
  if (Json(r.value()) != test_case["expectSelected"]) {
    return Outcome::fail("selected " + r.value() + ", expected " + test_case["expectSelected"].dump());
  }
  return Outcome::pass();
}

// --- evaluate_function --------------------------------------------------------

/** A surface that keeps the first evaluation error with its kind (the Python _RaisingSurfaceModel). */
class RaisingSurface {
 public:
  template <class... A>
  explicit RaisingSurface(A&&... a) : surface(std::forward<A>(a)...) {
    sub = surface.on_function_error().subscribe([this](const Error& e) {
      if (!first) first = e;
    });
  }
  a2ui::DataContext data_context(const std::string& p) { return surface.data_context(p); }
  SurfaceModel surface;
  std::optional<Error> first;
  a2ui::Subscription sub;
};

/** ValidationResult objects read as booleans (pre-1.0 validation functions). */
Json as_pre_v1(const Json& v) {
  if (v.is_object() && v["valid"].is_bool()) return v["valid"];
  return v;
}

bool has_pre_v1_binding(const Json& node, int depth = 0) {
  if (depth > 12) return false;
  if (node.is_object()) {
    if (node["path"].is_string() || node["call"].is_string()) return true;
    for (const auto& m : node.as_object()) {
      if (has_pre_v1_binding(m.second, depth + 1)) return true;
    }
  } else if (node.is_array()) {
    for (const auto& e : node.as_array()) {
      if (has_pre_v1_binding(e, depth + 1)) return true;
    }
  }
  return false;
}

HARNESS_ACTION("evaluate_function", run_evaluate_function) {
  if (Outcome u = unsupported_for(test_case); !u.passed()) return u;
  a2ui::locale::set_default_locale(test_case["locale"].is_string() ? test_case["locale"].as_string() : "en-US");
  const bool pre_v1 = !test_case.contains("protocolVersion");
  if (pre_v1 && has_pre_v1_binding(test_case["args"])) {
    return Outcome::unsupported("uses v0.9 bindings (path/call)");
  }
  const std::string fn = str(test_case["function"]);
  const Json* expect_error = harness::find_expect_error(test_case);

  Json value;
  std::optional<Error> error;
  if (test_case.contains("surface")) {
    a2ui::CatalogMap available;
    for (const auto& p : test_case["catalogPaths"].as_array()) {
      std::string err;
      Json doc = harness::load_json_file(ctx.repo_path(str(p)), &err);
      if (!doc.is_object()) return Outcome::fail(err);
      CatalogPtr c;
      if (str(doc["catalogId"]) == a2ui::basic_catalog::kCatalogIdV1) {
        c = basic_v1();
      } else {
        c = from_json_or_note(doc, str(doc["catalogId"]), str(doc["protocolVersion"]), &err);
        if (!c) return Outcome::fail(err);
      }
      if (c->protocol_version() == "1.0") available[c->id()] = c;
    }
    CatalogPtr def;
    if (test_case["surface"]["catalogId"].is_string()) def = available[test_case["surface"]["catalogId"].as_string()];
    a2ui::SurfaceOptions so;
    so.protocol_version = "1.0";
    so.data_model = test_case["dataModel"].is_object() ? test_case["dataModel"] : Json::object();
    RaisingSurface surface("main", def, available, so);
    a2ui::DataContext dc = surface.data_context("/");
    Json call = Json::object({{"@call", Json(fn)}, {"args", test_case["args"]}});
    if (test_case.contains("catalogId")) call.set("catalogId", test_case["catalogId"]);
    value = dc.resolve_value(call);
    error = surface.first;
  } else {
    CatalogPtr cat = basic_v1();
    a2ui::SurfaceOptions so;
    so.protocol_version = "1.0";
    so.data_model = test_case["dataModel"].is_object() ? test_case["dataModel"] : Json::object();
    RaisingSurface surface("main", cat, a2ui::CatalogMap{}, so);
    a2ui::DataContext dc = surface.data_context("/");
    const a2ui::FunctionDecl* decl = cat->find_function(fn);
    if (decl && decl->has_local_impl()) {
      auto r = decl->impl(test_case["args"], dc);
      if (r.ok()) {
        value = r.value();
      } else {
        error = r.error();
      }
    } else {
      value = dc.resolve_value(Json::object({{"@call", Json(fn)}, {"args", test_case["args"]}}));
      error = surface.first;
    }
    if (pre_v1) value = as_pre_v1(value);
  }
  if (expect_error) {
    if (!error) return Outcome::fail("expected an error, got " + value.dump());
    return check_full_error(*error, *expect_error, {});
  }
  if (error) return Outcome::fail("unexpected error: " + describe(*error));
  const Json& want = test_case["expect"];
  if (!(value == want)) {
    if (want.is_number() && value.is_number() && std::fabs(want.as_number() - value.as_number()) < 1e-9) {
      return Outcome::pass();
    }
    return Outcome::fail("got " + value.dump() + ", expected " + want.dump());
  }
  return Outcome::pass();
}

// --- dispatch_action ------------------------------------------------------------

HARNESS_ACTION("dispatch_action", run_dispatch_action) {
  if (Outcome u = unsupported_for(test_case); !u.passed()) return u;
  if (!test_case.contains("protocolVersion")) {
    const Json& payload = test_case["actionPayload"];
    const Json& context = payload["event"].is_object() ? payload["event"]["context"] : payload["context"];
    for (const auto& m : context.as_object()) {
      if (m.second.is_object() && has_pre_v1_binding(m.second)) return Outcome::unsupported("uses v0.9 bindings (path)");
    }
  }
  CatalogPtr cat = basic_alias(a2ui::basic_catalog::kCatalogIdV1, true);
  {
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
    cat->add_function(std::move(inc));
    a2ui::FunctionDecl fail;
    fail.api.name = "failingAction";
    fail.api.return_type = "void";
    fail.impl = [](const Json&, a2ui::FunctionContext&) -> a2ui::Result<Json> {
      return a2ui::make_error(ErrorKind::Generic, "Action execution failed");
    };
    cat->add_function(std::move(fail));
  }
  const std::string sid = test_case["surfaceId"].is_string() ? test_case["surfaceId"].as_string() : "main";
  const std::string source = test_case["sourceComponentId"].is_string() ? test_case["sourceComponentId"].as_string()
                                                                         : "test_comp";
  const bool activated = test_case["userActivation"].is_bool() ? test_case["userActivation"].as_bool() : true;
  a2ui::SurfaceOptions so;
  so.protocol_version = "1.0";
  so.data_model = test_case["dataModel"].is_object() ? test_case["dataModel"] : Json::object();
  SurfaceModel surface(sid, cat, a2ui::CatalogMap{}, so);
  std::vector<Json> dispatched, errors;
  auto s1 = surface.on_action().subscribe([&](const Json& a) { dispatched.push_back(a); });
  auto s2 = surface.on_error().subscribe([&](const Json& e) { errors.push_back(e); });
  const std::string scope = test_case["scope"].is_string() ? test_case["scope"].as_string() : "/";

  Json resolved = surface.resolve_action(test_case["actionPayload"], scope, activated);
  a2ui::Status st = a2ui::ok_status();
  if (resolved.is_object()) st = surface.dispatch_action(resolved, source, test_case["metadata"]);

  if (const Json* ee = harness::find_expect_error(test_case)) {
    std::optional<Error> err;
    if (!st.ok()) err = st.error();
    if (!err && !errors.empty()) {
      const Json& e = errors.back();
      Error x = a2ui::make_error(ErrorKind::Validation, str(e["message"]), str(e["code"]));
      x.surface_id = str(e["surfaceId"]);
      err = x;
    }
    if (!err) return harness::missing_error(*ee);
    std::vector<Json> renderer;
    for (const auto& e : errors) renderer.push_back(Json::object({{"version", Json("1.0")}, {"error", e}}));
    Outcome o = check_full_error(*err, *ee, renderer);
    if (!o.passed()) return o;
    if (test_case.contains("expectDispatched") && test_case["expectDispatched"].is_null() && !dispatched.empty()) {
      return Outcome::fail("an action was dispatched: " + dispatched[0].dump());
    }
    return Outcome::pass();
  }
  if (!st.ok()) return Outcome::fail(describe(st.error()));
  if (!errors.empty()) return Outcome::fail("surface error: " + errors[0].dump());
  if (test_case.contains("expectDispatched")) {
    const Json& want = test_case["expectDispatched"];
    if (want.is_null()) {
      if (!dispatched.empty()) return Outcome::fail("expected no action, got " + dispatched[0].dump());
    } else {
      if (dispatched.empty()) return Outcome::fail("no action was dispatched");
      const Json& got = dispatched[0];
      for (const char* k : {"name", "context", "userMessage", "surfaceId", "sourceComponentId", "metadata"}) {
        if (!want.contains(k)) continue;
        const Json g = got.contains(k) ? got[k] : Json(nullptr);
        if (!(g == want[k])) return Outcome::fail(std::string(k) + ": got " + g.dump() + ", expected " + want[k].dump());
      }
      if (want["timestampIso8601"].as_bool() || want["hasTimestamp"].as_bool()) {
        if (str(got["timestamp"]).empty()) return Outcome::fail("no timestamp");
      }
    }
  }
  if (test_case.contains("expectDataModel") && !(surface.data_model().root() == test_case["expectDataModel"])) {
    return Outcome::fail("data model " + surface.data_model().root().dump());
  }
  return Outcome::pass();
}

// --- measure_growth -------------------------------------------------------------

std::string expand_template(const Json& tpl, int n) {
  std::string out;
  for (const auto& part : tpl.as_array()) {
    if (part.is_string()) {
      out += part.as_string();
      continue;
    }
    const std::string rep = str(part["repeat"]);
    const std::string sep = str(part["separator"]);
    for (int i = 0; i < n; ++i) {
      if (i) out += sep;
      std::string r = rep;
      for (size_t pos; (pos = r.find("{i}")) != std::string::npos;) r.replace(pos, 3, std::to_string(i));
      out += r;
    }
  }
  return out;
}

HARNESS_ACTION("measure_growth", run_measure_growth) {
  if (Outcome u = unsupported_for(test_case); !u.passed()) return u;
  if (str(test_case["operation"]) != "process_messages") return Outcome::unsupported("unknown operation");
  CaseCatalogs cats;
  for (const auto& p : test_case["catalogPaths"].as_array()) {
    std::string err;
    Json doc = harness::load_json_file(ctx.repo_path(str(p)), &err);
    if (str(doc["catalogId"]) == a2ui::basic_catalog::kCatalogIdV1) {
      cats.list.push_back(basic_v1());
    } else if (CatalogPtr c = from_json_or_note(doc, str(doc["catalogId"]), str(doc["protocolVersion"]), &err)) {
      cats.list.push_back(c);
    } else {
      return Outcome::fail(err);
    }
  }
  const std::string mode = str(test_case["args"]["validation_config"]);
  std::optional<ValidationConfig> config;
  if (mode == "relaxed") config = ValidationConfig::relaxed();
  const auto& sizes = test_case["sizes"].as_array();
  const int n1 = static_cast<int>(sizes[0].as_number()), n2 = static_cast<int>(sizes[1].as_number());
  const Json p1 = Json::parse(expand_template(test_case["payload"], n1), 128).value;
  const Json p2 = Json::parse(expand_template(test_case["payload"], n2), 128).value;
  auto time_once = [&](const Json& payload, std::string* fail) {
    a2ui::MessageProcessorOptions o;
    o.validation_config = config;
    MessageProcessor p(cats.list, o);
    const auto t0 = std::chrono::steady_clock::now();
    a2ui::Status s = p.process_messages(payload);
    const auto t1 = std::chrono::steady_clock::now();
    if (!s.ok()) *fail = describe(s.error());
    return std::chrono::duration<double>(t1 - t0).count();
  };
  std::string fail;
  time_once(p1, &fail);
  if (!fail.empty()) return Outcome::fail(fail);
  std::vector<double> a, b;
  for (int i = 0; i < 5; ++i) {
    a.push_back(time_once(p1, &fail));
    b.push_back(time_once(p2, &fail));
  }
  if (!fail.empty()) return Outcome::fail(fail);
  std::sort(a.begin(), a.end());
  std::sort(b.begin(), b.end());
  const double ratio = b[2] / std::max(a[2], 1e-9);
  const double max_ratio = test_case["max_ratio"].as_number();
  if (ratio > max_ratio) {
    return Outcome::fail("growth ratio " + std::to_string(ratio) + " exceeds " + std::to_string(max_ratio));
  }
  return Outcome::pass();
}

}  // namespace

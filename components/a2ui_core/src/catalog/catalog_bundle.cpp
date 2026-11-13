// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Catalog::validation_schema: the self-contained, bundled JSON Schema of a
// catalog, in the shape of the conformance goldens
// (conformance/test_data/validation_schema/). Mirrors the Python core's
// Catalog.validation_schema:
//
// - Each component's allOf mixins (common_types ComponentCommon and
//   Checkable, and local $defs mixins) are flattened into its `properties`
//   and `required`; a property the component declares itself wins over a
//   mixin's. ComponentCommon's `@catalogId` is left out, and every component
//   gets the `id` envelope property.
// - Every common_types.json definition the result reaches is copied into
//   `$defs`, and references become local "#/$defs/..." pointers. The
//   catalog's own leaf `$defs` stay as written.
// - A component or function reference to a common type without a
//   `description` gets the common type's description.
// - `$defs.anyComponent` / `$defs.anyFunction` are kept as authored, or
//   derived from the entries (`{"not": {}}` for no functions).
// - Top level: $schema, catalogId, protocolVersion, instructions, theme,
//   components, functions, $defs. `$id`, title and description describe the
//   document and are dropped.
//
// This is used by agents and tooling (prompting, self-containment checks);
// renderers validate through schema_registry() without building it.

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "a2ui/catalog.h"
#include "a2ui/schema.h"
#include "a2ui/spec_documents.h"
#include "catalog_internal.h"

namespace a2ui {
namespace {

constexpr int kMaxDepth = 64;

struct Bundler {
  const Json* common = nullptr;      // common_types.json $defs, or null.
  std::set<std::string> own;         // Defs the catalog declares itself.
  std::set<std::string> from_common; // Common types copied (or to copy).
  std::vector<std::string> pending;

  void need(const std::string& def) {
    if (own.count(def) || from_common.count(def)) return;
    from_common.insert(def);
    pending.push_back(def);
  }

  static std::string def_name(std::string_view ptr) {
    // ptr = "/$defs/<name>[/...]"
    ptr.remove_prefix(7);
    return unescape(ptr.substr(0, ptr.find('/')));
  }

  static std::string unescape(std::string_view t) {
    std::string out;
    for (size_t i = 0; i < t.size(); ++i) {
      if (t[i] == '~' && i + 1 < t.size() && (t[i + 1] == '0' || t[i + 1] == '1')) {
        out.push_back(t[++i] == '0' ? '~' : '/');
      } else {
        out.push_back(t[i]);
      }
    }
    return out;
  }

  // Rewrites the references in `node`. `in_common` is true inside a copy
  // of a common type, whose "#/..." references point into common_types.json.
  void rewrite(Json& node, bool in_common, int depth) {
    if (depth > kMaxDepth) return;
    if (node.is_object()) {
      if (Json* ref = node.find_mut("$ref"); ref && ref->is_string()) {
        const std::string r = ref->as_string();
        std::string def, rest;
        std::string_view doc, ptr;
        split_ref(r, &doc, &ptr);
        if (catalog_internal::is_common_types_ref(r, &def, &rest)) {
          need(def);
          *ref = Json("#/$defs/" + escape_pointer_token(def) + rest);
        } else if (doc == "catalog.json") {
          *ref = Json("#" + std::string(ptr));
        } else if (doc.empty() && in_common && ptr.rfind("/$defs/", 0) == 0) {
          need(def_name(ptr));
        }
      }
      for (auto& m : node.as_object_mut()) rewrite(m.second, in_common, depth + 1);
    } else if (node.is_array()) {
      for (auto& e : node.as_array_mut()) rewrite(e, in_common, depth + 1);
    }
  }

  // Adds the description of the referenced common type to a reference
  // that has none.
  void propagate_descriptions(Json& node, int depth) {
    if (depth > kMaxDepth) return;
    if (node.is_object()) {
      const Json& ref = node["$ref"];
      if (ref.is_string() && !node.contains("description") &&
          ref.as_string().rfind("#/$defs/", 0) == 0) {
        const std::string name = def_name(std::string_view(ref.as_string()).substr(1));
        if (from_common.count(name) && common &&
            ref.as_string().find('/', 8) == std::string::npos) {
          const Json& desc = (*common)[name]["description"];
          if (desc.is_string()) node.set("description", desc);
        }
      }
      for (auto& m : node.as_object_mut()) propagate_descriptions(m.second, depth + 1);
    } else if (node.is_array()) {
      for (auto& e : node.as_array_mut()) propagate_descriptions(e, depth + 1);
    }
  }
};

void merge_shape(const Json& from, Json* props, Json* required) {
  for (const auto& m : from["properties"].as_object()) props->set(m.first, m.second);
  for (const auto& r : from["required"].as_array()) {
    bool seen = false;
    for (const auto& e : required->as_array()) seen = seen || e == r;
    if (!seen) required->push_back(r);
  }
}

// Flattens a component's allOf mixins into its properties / required.
Json flatten_component(const Json& schema, const Json& own_defs, Bundler* b, int depth) {
  Json flat = Json::object();
  for (const auto& m : schema.as_object()) {
    if (m.first != "allOf" && m.first != "properties" && m.first != "required") flat.set(m.first, m.second);
  }
  Json props = Json::object();
  Json required = Json::array();
  Json residual = Json::array();  // allOf members that are not mixins.
  // Mixins referenced by $ref first; inline members and the component's
  // own properties then override them.
  for (const auto& member : schema["allOf"].as_array()) {
    const Json& ref = member["$ref"];
    if (!ref.is_string()) continue;
    std::string def, rest;
    std::string_view doc, ptr;
    split_ref(ref.as_string(), &doc, &ptr);
    if (catalog_internal::is_common_types_ref(ref.as_string(), &def, &rest) && rest.empty() && b->common &&
        (*b->common)[def].is_object()) {
      Json mixin = (*b->common)[def];
      b->rewrite(mixin, true, 0);
      if (def == "ComponentCommon") {
        if (Json* p = mixin.find_mut("properties")) p->erase("@catalogId");
      }
      if (depth < 8) mixin = flatten_component(mixin, own_defs, b, depth + 1);
      merge_shape(mixin, &props, &required);
    } else if ((doc.empty() || doc == "catalog.json") && ptr.rfind("/$defs/", 0) == 0 &&
               ptr.find('/', 7) == std::string_view::npos &&
               own_defs[Bundler::def_name(ptr)].is_object()) {
      Json mixin = own_defs[Bundler::def_name(ptr)];
      if (depth < 8) mixin = flatten_component(mixin, own_defs, b, depth + 1);
      merge_shape(mixin, &props, &required);
    } else {
      residual.push_back(member);
    }
  }
  for (const auto& member : schema["allOf"].as_array()) {
    if (member["$ref"].is_string()) continue;
    merge_shape(member, &props, &required);
  }
  merge_shape(schema, &props, &required);
  if (!residual.empty()) flat.set("allOf", std::move(residual));
  if (!props.empty() || schema.contains("properties") || schema.contains("allOf")) {
    flat.set("properties", std::move(props));
  }
  if (!required.empty()) flat.set("required", std::move(required));
  return flat;
}

}  // namespace

Json Catalog::validation_schema() const {
  const Json doc = to_json();
  const std::shared_ptr<const Json> common_doc = spec::common_types(protocol_version_);
  const Json& own_defs = doc["$defs"];

  Bundler b;
  if (common_doc) b.common = &(*common_doc)["$defs"];
  for (const auto& m : own_defs.as_object()) b.own.insert(m.first);

  Json out = Json::object();
  out.set("$schema", Json("https://json-schema.org/draft/2020-12/schema"));
  out.set("catalogId", Json(id_));
  if (doc.contains("protocolVersion")) out.set("protocolVersion", doc["protocolVersion"]);
  if (doc["instructions"].is_string()) out.set("instructions", doc["instructions"]);
  if (doc.contains("theme")) {
    Json theme = doc["theme"];
    b.rewrite(theme, false, 0);
    out.set("theme", std::move(theme));
  }

  Json components = Json::object();
  for (const auto& m : doc["components"].as_object()) {
    Json flat = flatten_component(m.second, own_defs, &b, 0);
    Json* props = flat.find_mut("properties");
    if (!props) {
      flat.set("properties", Json::object());
      props = flat.find_mut("properties");
    }
    if (!props->contains("id")) {
      props->set("id", Json::object({{"$ref", Json("#/$defs/ComponentId")}}));
      b.need("ComponentId");
    }
    Json* req = flat.find_mut("required");
    if (!req) {
      flat.set("required", Json::array());
      req = flat.find_mut("required");
    }
    bool has_id = false;
    for (const auto& r : req->as_array()) has_id = has_id || (r.is_string() && r.as_string() == "id");
    if (!has_id) req->push_back(Json("id"));
    b.rewrite(flat, false, 0);
    components.set(m.first, std::move(flat));
  }
  out.set("components", std::move(components));

  if (doc["functions"].is_object() && !doc["functions"].empty()) {
    Json functions = doc["functions"];
    b.rewrite(functions, false, 0);
    out.set("functions", std::move(functions));
  }

  Json defs = Json::object();
  for (const auto& m : own_defs.as_object()) {
    if (m.first == "anyComponent" || m.first == "anyFunction") continue;
    Json d = m.second;
    b.rewrite(d, false, 0);
    defs.set(m.first, std::move(d));
  }
  Json any_component = own_defs["anyComponent"];
  if (any_component.is_undefined()) {
    std::vector<std::string> names;
    for (const auto& c : components_) names.push_back(c.name);
    any_component = catalog_internal::make_union(names, "#/components/", true);
  }
  b.rewrite(any_component, false, 0);
  defs.set("anyComponent", std::move(any_component));
  Json any_function = own_defs["anyFunction"];
  if (any_function.is_undefined()) {
    std::vector<std::string> names;
    for (const auto& f : functions_) {
      if (f.api.name.empty() || f.api.name[0] != '@') names.push_back(f.api.name);
    }
    any_function = names.empty() ? Json::object({{"not", Json::object()}})
                                  : catalog_internal::make_union(names, "#/functions/", false);
  }
  b.rewrite(any_function, false, 0);
  defs.set("anyFunction", std::move(any_function));

  while (!b.pending.empty()) {
    const std::string name = b.pending.back();
    b.pending.pop_back();
    const Json* def = b.common ? b.common->find(name) : nullptr;
    Json copy = def ? *def : Json::object({{"not", Json::object()}});
    b.rewrite(copy, true, 0);
    defs.set(name, std::move(copy));
  }

  if (Json* c = out.find_mut("components")) b.propagate_descriptions(*c, 0);
  if (Json* f = out.find_mut("functions")) b.propagate_descriptions(*f, 0);
  out.set("$defs", std::move(defs));
  return out;
}

}  // namespace a2ui

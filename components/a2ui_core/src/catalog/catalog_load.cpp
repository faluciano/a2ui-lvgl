// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Catalog::from_json and Catalog::from_packed: loading and load-time
// validation of a catalog document (specification "The component catalog";
// blueprint "Catalog Layer" and "Catalog Schema Validation"). Ported from
// the Python core's Catalog.from_json and the Swift core's catalog loading.
//
// Both loaders read the document through SchemaNode, so the checks and the
// analysis of each component are the same code for a Json document and a
// packed one; only where the schemas end up differs.

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "a2ui/catalog.h"
#include "a2ui/identifier.h"
#include "a2ui/metadata.h"
#include "a2ui/schema.h"
#include "a2ui/spec_documents.h"
#include "catalog_internal.h"

namespace a2ui {

std::string normalize_protocol_version(std::string_view v) {
  if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.remove_prefix(1);
  // major.minor[.patch][-pre][+build]
  auto read_num = [&v](long* out) {
    if (v.empty() || v[0] < '0' || v[0] > '9') return false;
    long n = 0;
    size_t i = 0;
    while (i < v.size() && v[i] >= '0' && v[i] <= '9' && n < 100000) n = n * 10 + (v[i++] - '0');
    v.remove_prefix(i);
    *out = n;
    return true;
  };
  long major = 0, minor = 0, patch = 0;
  if (!read_num(&major)) return {};
  if (v.empty() || v[0] != '.') return {};
  v.remove_prefix(1);
  if (!read_num(&minor)) return {};
  if (!v.empty() && v[0] == '.') {
    v.remove_prefix(1);
    if (!read_num(&patch)) return {};
  }
  if (!v.empty() && v[0] != '-' && v[0] != '+') return {};
  if (major == 1 && minor == 0) return "1.0";
  if (major == 0 && minor == 9) return "0.9";
  if (major == 0 && minor == 8) return "0.8";
  return {};
}

namespace catalog_internal {

bool is_common_types_ref(std::string_view ref, std::string* def_name, std::string* rest) {
  std::string_view doc, ptr;
  split_ref(ref, &doc, &ptr);
  const size_t slash = doc.rfind('/');
  const std::string_view base = slash == std::string_view::npos ? doc : doc.substr(slash + 1);
  if (base != "common_types.json") return false;
  constexpr std::string_view kDefs = "/$defs/";
  if (ptr.substr(0, kDefs.size()) != kDefs) return false;
  ptr.remove_prefix(kDefs.size());
  const size_t end = ptr.find('/');
  if (def_name) *def_name = std::string(ptr.substr(0, end));
  if (rest) *rest = end == std::string_view::npos ? std::string() : std::string(ptr.substr(end));
  return true;
}

std::vector<std::string> union_member_names(SchemaNode union_schema, std::string_view prefix) {
  std::vector<std::string> names;
  for (SchemaNode item : union_schema["oneOf"].items()) {
    const SchemaNode ref = item["$ref"];
    if (ref.is_string() && ref.as_string().substr(0, prefix.size()) == prefix) {
      const std::string_view r = ref.as_string();
      names.emplace_back(r.substr(r.rfind('/') + 1));
    }
  }
  return names;
}

Json make_union(const std::vector<std::string>& names, std::string_view prefix, bool discriminator) {
  Json one_of = Json::array();
  for (const auto& n : names) {
    one_of.push_back(Json::object({{"$ref", Json(std::string(prefix) + escape_pointer_token(n))}}));
  }
  Json u = Json::object({{"oneOf", std::move(one_of)}});
  if (discriminator) {
    u.set("discriminator", Json::object({{"propertyName", Json("component")}}));
  }
  return u;
}


namespace {

bool ends_with(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_single_child_schema(SchemaNode s) {
  if (!s.is_object()) return false;
  if (const SchemaNode r = s["$ref"]; r.is_string() && (ends_with(r.as_string(), "/ComponentId") ||
                                                        ends_with(r.as_string(), "/Child"))) {
    return true;
  }
  for (const char* kw : {"allOf", "oneOf", "anyOf"}) {
    for (SchemaNode sub : s[kw].items()) {
      if (is_single_child_schema(sub)) return true;
    }
  }
  return false;
}

bool is_child_list_schema(SchemaNode s) {
  if (!s.is_object()) return false;
  if (const SchemaNode r = s["$ref"]; r.is_string() && ends_with(r.as_string(), "/ChildList")) return true;
  for (const char* kw : {"allOf", "oneOf", "anyOf"}) {
    for (SchemaNode sub : s[kw].items()) {
      if (is_child_list_schema(sub)) return true;
    }
  }
  const SchemaNode props = s["properties"];
  if (props.is_object()) {
    if (props.contains("explicitList") || props.contains("template")) return true;
    if (props.contains("componentId") && (props.contains("path") || props.contains("dataBinding"))) return true;
  }
  return false;
}

SchemaNode resolve_local_ref(SchemaNode ref, SchemaNode local, SchemaNode catalog_doc) {
  if (!ref.is_string()) return {};
  const std::string_view r = ref.as_string();
  if (r.rfind("#/", 0) != 0) return {};
  const std::string_view ptr = r.substr(1);
  if (ptr.rfind("/$defs/", 0) == 0) {
    if (const SchemaNode t = resolve_node_pointer(local, ptr); t.is_object()) return t;
  }
  if (const SchemaNode t = resolve_node_pointer(catalog_doc, ptr); t.is_object()) return t;
  return {};
}

void add_unique(std::vector<std::string>* v, std::string_view s) {
  if (std::find(v->begin(), v->end(), s) == v->end()) v->emplace_back(s);
}

void inspect_child_schema(SchemaNode sub, SchemaNode root, SchemaNode catalog_doc, int depth,
                          ComponentApi::ChildRefs* out) {
  if (!sub.is_object() || depth > 8) return;
  for (const char* kw : {"allOf", "anyOf", "oneOf"}) {
    for (SchemaNode branch : sub[kw].items()) inspect_child_schema(branch, root, catalog_doc, depth + 1, out);
  }
  if (const SchemaNode t = resolve_local_ref(sub["$ref"], root, catalog_doc)) {
    inspect_child_schema(t, root, catalog_doc, depth + 1, out);
  }
  for (const auto& m : sub["properties"].members()) {
    const std::string_view name = m.key;
    if (name == "id" || name == "component") continue;
    const SchemaNode ps = m.value;
    if (is_single_child_schema(ps)) {
      add_unique(&out->single, name);
      continue;
    }
    if (is_child_list_schema(ps)) {
      add_unique(&out->list, name);
      continue;
    }
    SchemaNode target = ps;
    if (const SchemaNode t = resolve_local_ref(ps["$ref"], root, catalog_doc)) target = t;
    if (!target.is_object()) continue;
    const bool is_array = (target["type"].is_string() && target["type"].as_string() == "array") ||
                          target.contains("items");
    if (!is_array) continue;
    SchemaNode items = target.find("items");
    if (!items.is_object()) continue;
    if (is_single_child_schema(items) || is_child_list_schema(items)) {
      add_unique(&out->list, name);
      continue;
    }
    if (const SchemaNode t = resolve_local_ref(items["$ref"], root, catalog_doc)) items = t;
    if (is_single_child_schema(items) || is_child_list_schema(items)) {
      add_unique(&out->list, name);
      continue;
    }
    for (const auto& sk : items["properties"].members()) {
      if (is_single_child_schema(sk.value) || is_child_list_schema(sk.value)) {
        add_unique(&out->list, name);
        add_unique(&out->nested[std::string(name)], sk.key);
      }
    }
  }
}

}  // namespace

ComponentApi::ChildRefs analyze_child_refs(SchemaNode schema, SchemaNode catalog_doc) {
  ComponentApi::ChildRefs out;
  inspect_child_schema(schema, schema, catalog_doc, 0, &out);
  return out;
}


namespace {

bool ref_ends(SchemaNode ref, std::string_view suffix) {
  return ref.is_string() && ends_with(ref.as_string(), suffix);
}

bool is_standard_mixin_property(std::string_view name, SchemaNode ps, bool has_inline_id) {
  if (!ps.is_object()) return false;
  const SchemaNode ref = ps["$ref"];
  if (name == "accessibility") return ref_ends(ref, "/AccessibilityAttributes");
  if (name == "checks") {
    if (ref_ends(ref, "/Checkable")) return true;
    return ps["items"].is_object() && ref_ends(ps["items"]["$ref"], "/CheckRule");
  }
  if (name == "metadata") {
    if (ref_ends(ref, "/ComponentCommonMetadata")) return true;
    const SchemaNode props = ps["properties"];
    return has_inline_id && ps["type"].is_string() && ps["type"].as_string() == "object" && props.is_object() &&
           props.size() == 1 && props.contains("extensions");
  }
  if (name == "catalogId") return has_inline_id && ps["type"].is_string() && ps["type"].as_string() == "string";
  return false;
}

std::string local_def_name(SchemaNode ref) {
  if (!ref.is_string()) return {};
  const std::string_view r = ref.as_string();
  const size_t at = r.find("#/$defs/");
  if (at == std::string_view::npos) return {};
  if (at != 0 && r.substr(0, at) != "catalog.json") return {};
  const std::string_view name = r.substr(at + 8);
  return std::string(name.substr(0, name.find('/')));
}

void declared_blocks(SchemaNode schema, SchemaNode defs, std::vector<std::string>* visited, int depth,
                     std::vector<SchemaNode>* out) {
  if (!schema.is_object() || depth > 12) return;
  if (schema["properties"].is_object()) out->push_back(schema["properties"]);
  const std::string def = local_def_name(schema["$ref"]);
  if (!def.empty() && def != "ComponentCommon" && def != "Checkable" && defs.contains(def) &&
      std::find(visited->begin(), visited->end(), def) == visited->end()) {
    visited->push_back(def);
    declared_blocks(defs[def], defs, visited, depth + 1, out);
  }
  for (const char* kw : {"allOf", "anyOf", "oneOf"}) {
    for (SchemaNode b : schema[kw].items()) declared_blocks(b, defs, visited, depth + 1, out);
  }
}

bool references_checkable(SchemaNode schema, SchemaNode defs, std::vector<std::string>* visited, int depth) {
  if (!schema.is_object() || depth > 12) return false;
  const SchemaNode ref = schema["$ref"];
  if (ref_ends(ref, "/Checkable")) return true;
  const std::string def = local_def_name(ref);
  if (!def.empty() && defs.contains(def) && std::find(visited->begin(), visited->end(), def) == visited->end()) {
    visited->push_back(def);
    if (references_checkable(defs[def], defs, visited, depth + 1)) return true;
  }
  const SchemaNode props = schema["properties"];
  if (props.is_object()) {
    if (props.contains("@checks")) return true;
    if (props.contains("checks") && is_standard_mixin_property("checks", props["checks"], false)) return true;
  }
  for (SchemaNode sub : schema["allOf"].items()) {
    if (references_checkable(sub, defs, visited, depth + 1)) return true;
  }
  return false;
}

}  // namespace

void analyze_envelope(SchemaNode schema, SchemaNode catalog_doc, ComponentApi* api) {
  const SchemaNode defs = catalog_doc["$defs"];
  std::vector<std::string> visited;
  std::vector<SchemaNode> blocks;
  declared_blocks(schema, defs, &visited, 0, &blocks);
  api->custom_envelope_properties.clear();
  for (SchemaNode props : blocks) {
    const bool inline_id = props.contains("id");
    for (const char* key : {"metadata", "catalogId", "accessibility", "checks"}) {
      if (props.contains(key) && !is_standard_mixin_property(key, props[key], inline_id)) {
        add_unique(&api->custom_envelope_properties, key);
      }
    }
  }
  visited.clear();
  api->checkable = references_checkable(schema, defs, &visited, 0);
}

}  // namespace catalog_internal

namespace {

using catalog_internal::is_common_types_ref;

bool contains(const std::vector<std::string>& v, std::string_view s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

Error catalog_error(std::string msg) { return make_error(ErrorKind::Catalog, std::move(msg)); }

// A v1.0 catalog may reference itself ("#..." or "catalog.json") and
// common_types.json (relative or absolute) and nothing else.
bool is_supported_v10_ref(std::string_view ref) {
  std::string_view doc, ptr;
  split_ref(ref, &doc, &ptr);
  if (doc.empty() || doc == "catalog.json") return true;
  const size_t slash = doc.rfind('/');
  return (slash == std::string_view::npos ? doc : doc.substr(slash + 1)) == "common_types.json";
}

Status check_v10_external_refs(SchemaNode node, std::string& pointer, int depth) {
  if (depth > 64) return ok_status();
  if (node.is_object()) {
    const SchemaNode ref = node["$ref"];
    if (ref.is_string() && !is_supported_v10_ref(ref.as_string())) {
      return catalog_error("Unsupported external $ref '" + std::string(ref.as_string()) + "' at " + pointer +
                           "/$ref: a v1.0 catalog may only reference its own components and "
                           "functions (#/...) or common_types.json.");
    }
    for (const auto& m : node.members()) {
      const size_t mark = pointer.size();
      pointer += '/';
      pointer += escape_pointer_token(m.key);
      Status s = check_v10_external_refs(m.value, pointer, depth + 1);
      pointer.resize(mark);
      if (!s.ok()) return s;
    }
  } else if (node.is_array()) {
    for (size_t i = 0; i < node.size(); ++i) {
      const size_t mark = pointer.size();
      pointer += '/';
      pointer += std::to_string(i);
      Status s = check_v10_external_refs(node.at(i), pointer, depth + 1);
      pointer.resize(mark);
      if (!s.ok()) return s;
    }
  }
  return ok_status();
}

bool is_component_mixin_key(std::string_view key) {
  return key == "@catalogId" || key == "@accessibility" || key == "@metadata" || key == "@checks";
}

// The $defs a function's parameter names are read with: the argument
// schema's own `$defs`, which take precedence, then the catalog's.
struct ParamDefs {
  SchemaNode own;
  SchemaNode catalog;

  // `pointer` is "/<name>[/...]".
  SchemaNode resolve(std::string_view pointer) const {
    const size_t end = pointer.find('/', 1);
    const std::string_view head = pointer.substr(0, end);
    if (own.is_object() && resolve_node_pointer(own, head)) return resolve_node_pointer(own, pointer);
    return catalog.is_object() ? resolve_node_pointer(catalog, pointer) : SchemaNode();
  }
};

// Declared parameter names of a function argument schema (Python
// `_parameter_names`): properties of the schema, its allOf/anyOf/oneOf
// members and local "#/$defs/..." targets, or a shorthand map of name ->
// subschema.
void collect_property_names(SchemaNode node, const ParamDefs& defs, std::vector<std::string>* names,
                            int depth) {
  if (!node.is_object() || depth > 16) return;
  const SchemaNode ref = node["$ref"];
  constexpr std::string_view kLocal = "#/$defs/";
  if (ref.is_string() && ref.as_string().substr(0, kLocal.size()) == kLocal) {
    if (const SchemaNode t = defs.resolve(ref.as_string().substr(kLocal.size() - 1))) {
      collect_property_names(t, defs, names, depth + 1);
    }
  }
  for (const auto& m : node["properties"].members()) names->emplace_back(m.key);
  for (const char* kw : {"allOf", "anyOf", "oneOf"}) {
    for (SchemaNode member : node[kw].items()) {
      collect_property_names(member, defs, names, depth + 1);
    }
  }
}

std::vector<std::string> parameter_names(SchemaNode args_schema, SchemaNode catalog_defs) {
  std::vector<std::string> names;
  if (!args_schema.is_object()) return names;
  const ParamDefs defs{args_schema["$defs"], catalog_defs};
  if (args_schema["properties"].is_object() || args_schema["$ref"].is_string() ||
      args_schema["allOf"].is_array() || args_schema["anyOf"].is_array() ||
      args_schema["oneOf"].is_array()) {
    collect_property_names(args_schema, defs, &names, 0);
    return names;
  }
  // Shorthand parameter map: every value a subschema, no schema keywords.
  if (args_schema.empty() || args_schema["type"].is_string() || args_schema.contains("not")) {
    return names;
  }
  for (const auto& m : args_schema.members()) {
    if (!m.value.is_object() && !m.value.is_bool()) return {};
  }
  for (const auto& m : args_schema.members()) names.emplace_back(m.key);
  return names;
}

// ---------------------------------------------------------------------------
// Property classification

struct ClassifyCtx {
  SchemaNode catalog_defs;  // The catalog's $defs (or absent).
  SchemaNode common;        // common_types.json, or absent.
};

PropertyKind kind_of_common_def(std::string_view def) {
  if (def == "DynamicString") return PropertyKind::DynamicString;
  if (def == "DynamicNumber") return PropertyKind::DynamicNumber;
  if (def == "DynamicBoolean") return PropertyKind::DynamicBoolean;
  if (def == "DynamicStringList") return PropertyKind::DynamicStringList;
  if (def == "DynamicValue" || def == "DataBinding" || def == "FunctionCall") {
    return PropertyKind::DynamicValue;
  }
  if (def == "ComponentId" || def == "Child") return PropertyKind::ComponentId;
  if (def == "ChildList") return PropertyKind::ChildList;
  if (def == "Action") return PropertyKind::Action;
  if (def == "CheckRule") return PropertyKind::Checks;
  return PropertyKind::Static;
}

// Resolves a $ref met while classifying. `in_common` says which document
// "#/..." refers to. Returns the target and updates `in_common`.
SchemaNode follow_ref(const ClassifyCtx& ctx, std::string_view ref, bool* in_common, std::string* common_def) {
  std::string def, rest;
  if (is_common_types_ref(ref, &def, &rest)) {
    *in_common = true;
    if (common_def) *common_def = def;
    return ctx.common ? resolve_node_pointer(ctx.common, "/$defs/" + escape_pointer_token(def) + rest)
                      : SchemaNode();
  }
  std::string_view doc, ptr;
  split_ref(ref, &doc, &ptr);
  if (!doc.empty() && doc != "catalog.json") return {};
  if (*in_common && doc.empty()) {
    if (ptr.rfind("/$defs/", 0) == 0 && common_def) {
      *common_def = std::string(ptr.substr(7, ptr.find('/', 7) == std::string_view::npos
                                                 ? std::string_view::npos
                                                 : ptr.find('/', 7) - 7));
    }
    return ctx.common ? resolve_node_pointer(ctx.common, ptr) : SchemaNode();
  }
  *in_common = false;
  if (ptr.rfind("/$defs/", 0) == 0) {
    if (ctx.catalog_defs) {
      if (const SchemaNode local = resolve_node_pointer(ctx.catalog_defs, ptr.substr(6))) return local;
    }
    // A local name the catalog does not define is the common type of that name.
    *in_common = true;
    if (common_def) {
      const size_t end = ptr.find('/', 7);
      *common_def = std::string(ptr.substr(7, end == std::string_view::npos ? std::string_view::npos : end - 7));
    }
    return ctx.common ? resolve_node_pointer(ctx.common, ptr) : SchemaNode();
  }
  return {};
}

PropertyKind classify_property(const ClassifyCtx& ctx, SchemaNode prop, bool in_common, int depth) {
  if (!prop.is_object() || depth > 8) return PropertyKind::Static;
  if (const SchemaNode ref = prop["$ref"]; ref.is_string()) {
    bool c = in_common;
    std::string def;
    const SchemaNode target = follow_ref(ctx, ref.as_string(), &c, &def);
    if (!def.empty()) {
      const PropertyKind k = kind_of_common_def(def);
      if (k != PropertyKind::Static) return k;
    }
    if (target) return classify_property(ctx, target, c, depth + 1);
    return PropertyKind::Static;
  }
  if (const SchemaNode items = prop["items"]; items.is_object() && items["$ref"].is_string()) {
    bool c = in_common;
    std::string def;
    follow_ref(ctx, items["$ref"].as_string(), &c, &def);
    if (def == "CheckRule") return PropertyKind::Checks;
  }
  // An inline union with a DataBinding or FunctionCall branch is dynamic. Its
  // literal branches give the type it coerces to: {type: boolean} next to the
  // binding makes it a DynamicBoolean, as the common Dynamic* defs are.
  for (const char* kw : {"oneOf", "anyOf"}) {
    bool dynamic = false;
    PropertyKind literal = PropertyKind::DynamicValue;
    bool mixed = false;  // Literal branches of more than one kind.
    bool any_literal = false;
    for (SchemaNode member : prop[kw].items()) {
      if (member["$ref"].is_string()) {
        bool c = in_common;
        std::string def;
        follow_ref(ctx, member["$ref"].as_string(), &c, &def);
        dynamic = dynamic || def == "DataBinding" || def == "FunctionCall";
        continue;
      }
      const SchemaNode type = member["type"];
      if (!type.is_string()) continue;
      const std::string_view t = type.as_string();
      PropertyKind k = PropertyKind::DynamicValue;
      if (t == "boolean") k = PropertyKind::DynamicBoolean;
      else if (t == "string") k = PropertyKind::DynamicString;
      else if (t == "number" || t == "integer") k = PropertyKind::DynamicNumber;
      else if (t == "array" && member["items"]["type"].is_string() && member["items"]["type"].as_string() == "string")
        k = PropertyKind::DynamicStringList;
      mixed = mixed || (any_literal && k != literal);
      literal = k;
      any_literal = true;
    }
    if (dynamic) return any_literal && !mixed ? literal : PropertyKind::DynamicValue;
  }
  return PropertyKind::Static;
}

// Collects the declared properties of an object schema (following $ref and
// allOf/anyOf/oneOf) and classifies each.
void collect_object_kinds(const ClassifyCtx& ctx, SchemaNode schema, bool in_common, int depth,
                          std::map<std::string, PropertyKind>* out) {
  if (!schema.is_object() || depth > 8) return;
  for (const auto& m : schema["properties"].members()) {
    const std::string key(m.key);
    auto it = out->find(key);
    if (it != out->end() && it->second != PropertyKind::Static) continue;
    (*out)[key] = classify_property(ctx, m.value, in_common, 0);
  }
  if (const SchemaNode ref = schema["$ref"]; ref.is_string()) {
    bool c = in_common;
    if (const SchemaNode t = follow_ref(ctx, ref.as_string(), &c, nullptr)) {
      collect_object_kinds(ctx, t, c, depth + 1, out);
    }
  }
  for (const char* kw : {"allOf", "anyOf", "oneOf"}) {
    for (SchemaNode member : schema[kw].items()) collect_object_kinds(ctx, member, in_common, depth + 1, out);
  }
}

// The inner property kinds of a Static property: its own declared
// properties, or those of its array items.
void collect_nested_kinds(const ClassifyCtx& ctx, SchemaNode prop, bool in_common, int depth,
                          std::map<std::string, PropertyKind>* out) {
  if (!prop.is_object() || depth > 8) return;
  collect_object_kinds(ctx, prop, in_common, depth, out);
  if (const SchemaNode items = prop["items"]; items.is_object()) collect_object_kinds(ctx, items, in_common, depth + 1, out);
  if (const SchemaNode ref = prop["$ref"]; ref.is_string()) {
    bool c = in_common;
    if (const SchemaNode t = follow_ref(ctx, ref.as_string(), &c, nullptr)) {
      if (const SchemaNode items = t["items"]; items.is_object()) collect_object_kinds(ctx, items, c, depth + 1, out);
    }
  }
}

void collect_shape(const ClassifyCtx& ctx, SchemaNode schema, bool in_common, int depth, ComponentApi* api) {
  if (!schema.is_object() || depth > 8) return;
  for (const auto& m : schema["properties"].members()) {
    if (m.key == "id" || m.key == "component") continue;
    const std::string key(m.key);
    if (api->property_kinds.count(key) && api->property_kinds[key] != PropertyKind::Static) {
      continue;  // The first declaration that binds wins (component before mixin).
    }
    const PropertyKind kind = classify_property(ctx, m.value, in_common, 0);
    api->property_kinds[key] = kind;
    if (kind == PropertyKind::Static) {
      std::map<std::string, PropertyKind> inner;
      collect_nested_kinds(ctx, m.value, in_common, 0, &inner);
      bool any = false;
      for (const auto& k : inner) any = any || k.second != PropertyKind::Static;
      if (any) api->nested_kinds[key] = std::move(inner);
    }
  }
  for (SchemaNode r : schema["required"].items()) {
    if (!r.is_string() || r.as_string() == "id" || r.as_string() == "component") continue;
    if (!contains(api->required, r.as_string())) api->required.emplace_back(r.as_string());
  }
  if (const SchemaNode ref = schema["$ref"]; ref.is_string()) {
    bool c = in_common;
    if (const SchemaNode t = follow_ref(ctx, ref.as_string(), &c, nullptr)) {
      collect_shape(ctx, t, c, depth + 1, api);
    }
  }
  for (SchemaNode member : schema["allOf"].items()) {
    collect_shape(ctx, member, in_common, depth + 1, api);
  }
}

Status check_property_identifiers(SchemaNode schema, const std::string& component, int depth) {
  if (!schema.is_object() || depth > 4) return ok_status();
  for (const auto& m : schema["properties"].members()) {
    const std::string key(m.key);
    if (!is_component_mixin_key(key) && !is_uax31_identifier(key)) {
      return catalog_error("Invalid UAX #31 property identifier: '" + key + "' in component '" + component + "'");
    }
  }
  for (SchemaNode member : schema["allOf"].items()) {
    A2UI_RETURN_IF_ERROR(check_property_identifiers(member, component, depth + 1));
  }
  return ok_status();
}

std::string declared_return_type(SchemaNode spec) {
  if (spec["returnType"].is_string()) return std::string(spec["returnType"].as_string());
  const SchemaNode rt = spec["properties"]["returnType"];
  if (rt["const"].is_string()) return std::string(rt["const"].as_string());
  return "any";
}

std::vector<std::string> string_list(SchemaNode list) {
  std::vector<std::string> v;
  for (SchemaNode p : list.items()) {
    if (p.is_string()) v.emplace_back(p.as_string());
  }
  return v;
}

// The summary of one component (everything but where its schema lives).
void summarize_component(const ClassifyCtx& ctx, SchemaNode schema, SchemaNode catalog_doc, ComponentApi* api) {
  collect_shape(ctx, schema, false, 0, api);
  if (schema["allowedParents"].is_array()) api->allowed_parents = string_list(schema["allowedParents"]);
  if (schema["allowedChildren"].is_array()) api->allowed_children = string_list(schema["allowedChildren"]);
  if (const SchemaNode md = schema.find("metadata")) api->metadata = md.to_json();
  api->child_refs = catalog_internal::analyze_child_refs(schema, catalog_doc);
  catalog_internal::analyze_envelope(schema, catalog_doc, api);
}

// The summary of one function (everything but where its schema lives).
void summarize_function(SchemaNode spec, FunctionApi* api) {
  api->return_type = declared_return_type(spec);
  const std::string callers = spec["allowedCallers"].is_string() ? std::string(spec["allowedCallers"].as_string()) : "";
  api->allowed_callers = callers == "agentOnly"         ? AllowedCallers::AgentOnly
                         : callers == "rendererOrAgent" ? AllowedCallers::RendererOrAgent
                                                        : AllowedCallers::RendererOnly;
  api->requires_user_activation = spec["requiresUserActivation"].as_bool();
}

SchemaNode args_of(SchemaNode spec) {
  if (spec["properties"]["args"].is_object()) return spec["properties"]["args"];
  if (spec["args"].is_object()) return spec["args"];
  if (spec["parameters"].is_object()) return spec["parameters"];
  return {};
}

}  // namespace

namespace catalog_internal {

// The checks both loaders run before building anything. Fills the
// resolved id, versions, permitted entry names and the function entries.
struct Checked {
  std::string id;
  std::string declared;
  std::string version;
  std::vector<std::string> permitted_components;
  std::vector<std::string> permitted_functions;
  std::vector<std::pair<std::string, SchemaNode>> functions;
  bool function_map = false;
};

Status check_catalog(SchemaNode document, std::string_view catalog_id, std::string_view protocol_version,
                     Checked* out) {
  if (!document.is_object()) return catalog_error("Catalog document must be a JSON object.");

  std::string id(catalog_id);
  if (id.empty() && document["catalogId"].is_string()) id = std::string(document["catalogId"].as_string());
  if (id.empty()) return catalog_error("catalog_id must be provided or exist in catalog schema.");

  std::string declared;
  if (document["protocolVersion"].is_string()) declared = std::string(document["protocolVersion"].as_string());
  std::string requested(protocol_version);
  if (requested.empty()) requested = declared.empty() ? std::string("0.9") : declared;
  const std::string version = normalize_protocol_version(requested);
  if (version.empty()) {
    return catalog_error("Catalog '" + id + "' declares an unsupported protocolVersion '" + requested + "'.");
  }
  const bool v1 = version == "1.0";

  if (v1) {
    std::string pointer;
    A2UI_RETURN_IF_ERROR(check_v10_external_refs(document, pointer, 0));
  }

  const SchemaNode components_in = document["components"];
  if (components_in.present() && !components_in.is_object()) {
    return catalog_error("Catalog 'components' must be an object mapping names to schemas.");
  }
  const SchemaNode functions_in = document["functions"];
  if (functions_in.present() && !functions_in.is_object() && !functions_in.is_array()) {
    return catalog_error("Catalog '" + id + "' 'functions' must be an object or a list.");
  }
  const SchemaNode defs = document["$defs"];
  out->permitted_components = union_member_names(defs["anyComponent"], "#/components/");
  out->permitted_functions = union_member_names(defs["anyFunction"], "#/functions/");

  for (const auto& m : components_in.members()) {
    const std::string name(m.key);
    if (!m.value.is_object()) {
      return catalog_error("Component '" + name + "' schema must be a JSON schema object.");
    }
    if (!v1) continue;
    if (!is_uax31_identifier(name)) {
      return catalog_error("Invalid UAX #31 component identifier: '" + name + "'");
    }
    if (name == "Surface") {
      return catalog_error("Component name 'Surface' is reserved for the surface container.");
    }
    A2UI_RETURN_IF_ERROR(check_property_identifiers(m.value, name, 0));
    if (const SchemaNode md = m.value.find("metadata")) {
      if (!md.is_object()) {
        return make_error(ErrorKind::Validation,
                          "Expected component '" + name + "' metadata to be an object, got " + md.dump());
      }
      A2UI_RETURN_IF_ERROR(validate_metadata(md.to_json(), "component '" + name + "' metadata"));
    }
  }

  // Functions: the map form, or the v0.9 list form of {name, ...}.
  auto& fn_entries = out->functions;
  out->function_map = functions_in.is_object();
  if (functions_in.is_object()) {
    for (const auto& m : functions_in.members()) {
      if (!m.value.is_object()) {
        return catalog_error("Catalog '" + id + "' function '" + std::string(m.key) +
                             "' must be a JSON schema object.");
      }
      fn_entries.emplace_back(std::string(m.key), m.value);
    }
  } else if (functions_in.is_array()) {
    for (size_t i = 0; i < functions_in.size(); ++i) {
      const SchemaNode e = functions_in.at(i);
      if (!e.is_object() || !e["name"].is_string() || e["name"].as_string().empty()) {
        return catalog_error("Catalog '" + id + "' function definition " + std::to_string(i) +
                             " is missing a non-empty 'name' string.");
      }
      for (const auto& prev : fn_entries) {
        if (prev.first == e["name"].as_string()) {
          return catalog_error("Catalog '" + id + "' function definition " + std::to_string(i) +
                               " duplicates the name '" + prev.first + "'.");
        }
      }
      fn_entries.emplace_back(std::string(e["name"].as_string()), e);
    }
  }
  for (const auto& entry : fn_entries) {
    const std::string& name = entry.first;
    const SchemaNode spec = entry.second;
    if (!v1) continue;
    if (!is_uax31_identifier(name)) {
      return catalog_error("Invalid UAX #31 function identifier: '" + name + "'" +
                           (name.size() > 0 && name[0] == '@'
                                ? " ('@' names are reserved for protocol system functions)"
                                : ""));
    }
    const SchemaNode props = spec["properties"];
    const SchemaNode args = args_of(spec);
    std::vector<std::string> params;
    if (args) {
      params = parameter_names(args, defs);
    } else if (props.is_object() && !props.contains("call") && !props.contains("@call") &&
               !props.contains("function")) {
      params = parameter_names(spec, defs);
    }
    for (const auto& p : params) {
      if (!is_uax31_identifier(p)) {
        return catalog_error("Invalid UAX #31 argument identifier: '" + p + "' in function '" + name + "'");
      }
    }
    const SchemaNode callers = spec["allowedCallers"];
    if (callers.present()) {
      const std::string c = callers.is_string() ? std::string(callers.as_string()) : std::string();
      if (c != "rendererOnly" && c != "agentOnly" && c != "rendererOrAgent") {
        return catalog_error("Function '" + name + "' declares an invalid allowedCallers " + callers.dump() + ".");
      }
      if (spec["requiresUserActivation"].as_bool() && c != "rendererOnly") {
        return catalog_error("Function '" + name + "' declares requiresUserActivation=true with allowedCallers='" +
                             c + "'; requiresUserActivation=true requires allowedCallers='rendererOnly'.");
      }
    }
  }

  out->id = std::move(id);
  out->declared = declared.empty() ? std::string(protocol_version) : declared;
  out->version = version;
  return ok_status();
}

}  // namespace catalog_internal

Result<CatalogPtr> Catalog::from_json(const Json& document, std::string_view catalog_id,
                                      std::string_view protocol_version) {
  catalog_internal::Checked checked;
  A2UI_RETURN_IF_ERROR(catalog_internal::check_catalog(document, catalog_id, protocol_version, &checked));
  // The document is copied once; the loaded definitions are then moved out
  // of the copy.
  return build_from_json(Json(document), std::move(checked));
}

Result<CatalogPtr> Catalog::from_json(Json&& document, std::string_view catalog_id,
                                      std::string_view protocol_version) {
  catalog_internal::Checked checked;
  A2UI_RETURN_IF_ERROR(catalog_internal::check_catalog(document, catalog_id, protocol_version, &checked));
  // Moving the root keeps every member where it is, so the nodes `checked`
  // holds stay valid.
  return build_from_json(std::move(document), std::move(checked));
}

Result<CatalogPtr> Catalog::build_from_json(Json document, catalog_internal::Checked checked) {
  auto catalog = make_ref<Catalog>();
  catalog->id_ = checked.id;
  catalog->protocol_version_ = checked.version;
  catalog->declared_protocol_version_ = checked.declared;
  catalog->source_ = std::move(document);
  Json& src = catalog->source_;
  const Ref<const Json> common = spec::common_types(checked.version);
  const ClassifyCtx ctx{SchemaNode(src["$defs"]), SchemaNode(common.get())};

  if (Json* comps = src.find_mut("components")) {
    Json::Object& members = comps->as_object_mut();
    Json::Object kept;
    for (auto& m : members) {
      if (!checked.permitted_components.empty() && !contains(checked.permitted_components, m.first)) {
        kept.push_back(std::move(m));
        continue;
      }
      ComponentApi api;
      api.name = m.first;
      api.schema = std::move(m.second);
      summarize_component(ctx, api.schema, src, &api);
      catalog->components_.push_back(std::move(api));
    }
    members = std::move(kept);
  }

  Json::Object kept_functions;
  for (auto& entry : checked.functions) {
    const std::string& name = entry.first;
    if (!checked.permitted_functions.empty() && !contains(checked.permitted_functions, name)) continue;
    FunctionDecl decl;
    FunctionApi& api = decl.api;
    api.name = name;
    if (checked.function_map) {
      Json* slot = src.find_mut("functions")->find_mut(name);
      // A repeated key leaves an already moved-out slot; copy the entry then.
      if (slot && !slot->is_undefined()) {
        api.schema = std::move(*slot);
      } else {
        api.schema = entry.second.to_json();
      }
    } else {
      api.schema = entry.second.to_json();  // List form (v0.9): kept in the document as written.
      api.schema.erase("name");
    }
    const Json& spec = api.schema;
    summarize_function(spec, &api);
    if (const SchemaNode args = args_of(spec)) api.args_schema = *args.json();
    if (spec["description"].is_string()) api.description = spec["description"];
    catalog->functions_.push_back(std::move(decl));
  }
  if (checked.function_map) {
    // Drop the moved-out (now undefined) slots; unloaded entries stay.
    Json::Object& members = src.find_mut("functions")->as_object_mut();
    for (auto& m : members) {
      if (!m.second.is_undefined()) kept_functions.push_back(std::move(m));
    }
    members = std::move(kept_functions);
  }
  return catalog;
}

Result<CatalogPtr> Catalog::from_packed(const PackedDocument& document, std::string_view catalog_id,
                                        std::string_view protocol_version) {
  const SchemaNode root = SchemaNode::root(document);
  catalog_internal::Checked checked;
  A2UI_RETURN_IF_ERROR(catalog_internal::check_catalog(root, catalog_id, protocol_version, &checked));

  // Build. Schemas stay in the packed document; only the summaries are copied.
  auto catalog = make_ref<Catalog>();
  catalog->id_ = checked.id;
  catalog->protocol_version_ = checked.version;
  catalog->declared_protocol_version_ = checked.declared;
  catalog->packed_ = &document;
  const PackedDocument* common = spec::packed_common_types(checked.version);
  catalog->packed_common_ = common;
  const ClassifyCtx ctx{root["$defs"], common ? SchemaNode::root(*common) : SchemaNode()};

  for (const auto& m : root["components"].members()) {
    if (!checked.permitted_components.empty() && !contains(checked.permitted_components, m.key)) continue;
    ComponentApi api;
    api.name = std::string(m.key);
    api.packed_schema = m.value;
    summarize_component(ctx, m.value, root, &api);
    catalog->components_.push_back(std::move(api));
  }

  for (const auto& entry : checked.functions) {
    const std::string& name = entry.first;
    if (!checked.permitted_functions.empty() && !contains(checked.permitted_functions, name)) continue;
    FunctionDecl decl;
    FunctionApi& api = decl.api;
    api.name = name;
    if (checked.function_map) {
      api.packed_schema = entry.second;
      api.packed_args_schema = args_of(entry.second);
    } else {
      api.schema = entry.second.to_json();  // List form (v0.9): materialized without its name.
      api.schema.erase("name");
      if (const SchemaNode args = args_of(api.schema)) api.args_schema = *args.json();
      if (api.schema["description"].is_string()) api.description = api.schema["description"];
    }
    summarize_function(entry.second, &api);
    catalog->functions_.push_back(std::move(decl));
  }
  return catalog;
}

}  // namespace a2ui

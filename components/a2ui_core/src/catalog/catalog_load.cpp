// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Catalog::from_json: loading and load-time validation of a catalog
// document (specification "The component catalog"; blueprint "Catalog
// Layer" and "Catalog Schema Validation"). Ported from the Python core's
// Catalog.from_json and the Swift core's catalog loading.

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "a2ui/catalog.h"
#include "a2ui/identifier.h"
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

std::vector<std::string> union_member_names(const Json& union_schema, std::string_view prefix) {
  std::vector<std::string> names;
  const Json& members = union_schema["oneOf"];
  for (const auto& item : members.as_array()) {
    const Json& ref = item["$ref"];
    if (ref.is_string() && ref.as_string().compare(0, prefix.size(), prefix) == 0) {
      const std::string& r = ref.as_string();
      names.push_back(r.substr(r.rfind('/') + 1));
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

Status check_v10_external_refs(const Json& node, std::string& pointer, int depth) {
  if (depth > 64) return ok_status();
  if (node.is_object()) {
    const Json& ref = node["$ref"];
    if (ref.is_string() && !is_supported_v10_ref(ref.as_string())) {
      return catalog_error("Unsupported external $ref '" + ref.as_string() + "' at " + pointer +
                           "/$ref: a v1.0 catalog may only reference its own components and "
                           "functions (#/...) or common_types.json.");
    }
    for (const auto& m : node.as_object()) {
      const size_t mark = pointer.size();
      pointer += '/';
      pointer += escape_pointer_token(m.first);
      Status s = check_v10_external_refs(m.second, pointer, depth + 1);
      pointer.resize(mark);
      if (!s.ok()) return s;
    }
  } else if (node.is_array()) {
    for (size_t i = 0; i < node.size(); ++i) {
      const size_t mark = pointer.size();
      pointer += '/';
      pointer += std::to_string(i);
      Status s = check_v10_external_refs(node[i], pointer, depth + 1);
      pointer.resize(mark);
      if (!s.ok()) return s;
    }
  }
  return ok_status();
}

// ComponentDefinition.metadata / ComponentCommon.@metadata: only
// `extensions`, whose keys are UAX #31 identifiers.
Status validate_metadata(const Json& metadata, const std::string& context) {
  if (!metadata.is_object()) {
    return make_error(ErrorKind::Validation,
                      "Expected " + context + " to be an object, got " + metadata.dump());
  }
  for (const auto& m : metadata.as_object()) {
    if (m.first != "extensions") {
      return make_error(ErrorKind::Validation, "Unexpected property '" + m.first + "' in " +
                                                   context + "; only 'extensions' is allowed.");
    }
    if (!m.second.is_object()) {
      return make_error(ErrorKind::Validation,
                        "Expected " + context + ".extensions to be an object.");
    }
    for (const auto& e : m.second.as_object()) {
      if (!is_uax31_identifier(e.first)) {
        return make_error(ErrorKind::Validation, "Invalid UAX #31 extension identifier '" +
                                                     e.first + "' in " + context + ".extensions.");
      }
    }
  }
  return ok_status();
}

bool is_component_mixin_key(std::string_view key) {
  return key == "@catalogId" || key == "@accessibility" || key == "@metadata" || key == "@checks";
}

// Declared parameter names of a function argument schema (Python
// `_parameter_names`): properties of the schema, its allOf/anyOf/oneOf
// members and local "#/$defs/..." targets, or a shorthand map of name ->
// subschema.
void collect_property_names(const Json& node, const Json& defs, std::vector<std::string>* names,
                            int depth) {
  if (!node.is_object() || depth > 16) return;
  const Json& ref = node["$ref"];
  constexpr std::string_view kLocal = "#/$defs/";
  if (ref.is_string() && ref.as_string().compare(0, kLocal.size(), kLocal) == 0) {
    if (const Json* t = resolve_json_pointer(defs, ref.as_string().substr(kLocal.size() - 1))) {
      collect_property_names(*t, defs, names, depth + 1);
    }
  }
  for (const auto& m : node["properties"].as_object()) names->push_back(m.first);
  for (const char* kw : {"allOf", "anyOf", "oneOf"}) {
    for (const auto& member : node[kw].as_array()) {
      collect_property_names(member, defs, names, depth + 1);
    }
  }
}

std::vector<std::string> parameter_names(const Json& args_schema, const Json& catalog_defs) {
  std::vector<std::string> names;
  if (!args_schema.is_object()) return names;
  Json defs = catalog_defs.is_object() ? catalog_defs : Json::object();
  for (const auto& m : args_schema["$defs"].as_object()) defs.set(m.first, m.second);
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
  for (const auto& m : args_schema.as_object()) {
    if (!m.second.is_object() && !m.second.is_bool()) return {};
  }
  for (const auto& m : args_schema.as_object()) names.push_back(m.first);
  return names;
}

// ---------------------------------------------------------------------------
// Property classification

struct ClassifyCtx {
  const Json& catalog_defs;  // The catalog's $defs (or undefined).
  const Json* common;        // common_types.json, or null.
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
const Json* follow_ref(const ClassifyCtx& ctx, const std::string& ref, bool* in_common,
                       std::string* common_def) {
  std::string def, rest;
  if (is_common_types_ref(ref, &def, &rest)) {
    *in_common = true;
    if (common_def) *common_def = def;
    return ctx.common ? resolve_json_pointer(*ctx.common, "/$defs/" + escape_pointer_token(def) + rest)
                      : nullptr;
  }
  std::string_view doc, ptr;
  split_ref(ref, &doc, &ptr);
  if (!doc.empty() && doc != "catalog.json") return nullptr;
  if (*in_common && doc.empty()) {
    if (ptr.rfind("/$defs/", 0) == 0 && common_def) {
      *common_def = std::string(ptr.substr(7, ptr.find('/', 7) == std::string_view::npos
                                                 ? std::string_view::npos
                                                 : ptr.find('/', 7) - 7));
    }
    return ctx.common ? resolve_json_pointer(*ctx.common, ptr) : nullptr;
  }
  *in_common = false;
  if (ptr.rfind("/$defs/", 0) == 0) return resolve_json_pointer(ctx.catalog_defs, ptr.substr(6));
  return nullptr;
}

PropertyKind classify_property(const ClassifyCtx& ctx, const Json& prop, bool in_common, int depth) {
  if (!prop.is_object() || depth > 8) return PropertyKind::Static;
  if (const Json& ref = prop["$ref"]; ref.is_string()) {
    bool c = in_common;
    std::string def;
    const Json* target = follow_ref(ctx, ref.as_string(), &c, &def);
    if (!def.empty()) {
      const PropertyKind k = kind_of_common_def(def);
      if (k != PropertyKind::Static) return k;
    }
    if (target) return classify_property(ctx, *target, c, depth + 1);
    return PropertyKind::Static;
  }
  if (const Json& items = prop["items"]; items.is_object() && items["$ref"].is_string()) {
    bool c = in_common;
    std::string def;
    follow_ref(ctx, items["$ref"].as_string(), &c, &def);
    if (def == "CheckRule") return PropertyKind::Checks;
  }
  for (const char* kw : {"oneOf", "anyOf"}) {
    for (const auto& member : prop[kw].as_array()) {
      if (!member["$ref"].is_string()) continue;
      bool c = in_common;
      std::string def;
      follow_ref(ctx, member["$ref"].as_string(), &c, &def);
      if (def == "DataBinding" || def == "FunctionCall") return PropertyKind::DynamicValue;
    }
  }
  return PropertyKind::Static;
}

void collect_shape(const ClassifyCtx& ctx, const Json& schema, bool in_common, int depth,
                   ComponentApi* api) {
  if (!schema.is_object() || depth > 8) return;
  for (const auto& m : schema["properties"].as_object()) {
    if (m.first == "id" || m.first == "component") continue;
    if (api->property_kinds.count(m.first) && api->property_kinds[m.first] != PropertyKind::Static) {
      continue;  // The first declaration that binds wins (component before mixin).
    }
    api->property_kinds[m.first] = classify_property(ctx, m.second, in_common, 0);
  }
  for (const auto& r : schema["required"].as_array()) {
    if (!r.is_string() || r.as_string() == "id" || r.as_string() == "component") continue;
    if (!contains(api->required, r.as_string())) api->required.push_back(r.as_string());
  }
  if (const Json& ref = schema["$ref"]; ref.is_string()) {
    bool c = in_common;
    if (const Json* t = follow_ref(ctx, ref.as_string(), &c, nullptr)) {
      collect_shape(ctx, *t, c, depth + 1, api);
    }
  }
  for (const auto& member : schema["allOf"].as_array()) {
    collect_shape(ctx, member, in_common, depth + 1, api);
  }
}

Status check_property_identifiers(const Json& schema, const std::string& component, int depth) {
  if (!schema.is_object() || depth > 4) return ok_status();
  for (const auto& m : schema["properties"].as_object()) {
    if (!is_component_mixin_key(m.first) && !is_uax31_identifier(m.first)) {
      return catalog_error("Invalid UAX #31 property identifier: '" + m.first +
                           "' in component '" + component + "'");
    }
  }
  for (const auto& member : schema["allOf"].as_array()) {
    A2UI_RETURN_IF_ERROR(check_property_identifiers(member, component, depth + 1));
  }
  return ok_status();
}

std::string declared_return_type(const Json& spec) {
  if (spec["returnType"].is_string()) return spec["returnType"].as_string();
  const Json& rt = spec["properties"]["returnType"];
  if (rt["const"].is_string()) return rt["const"].as_string();
  return "any";
}

}  // namespace

Result<CatalogPtr> Catalog::from_json(const Json& document, std::string_view catalog_id,
                                      std::string_view protocol_version) {
  if (!document.is_object()) return catalog_error("Catalog document must be a JSON object.");

  std::string id(catalog_id);
  if (id.empty() && document["catalogId"].is_string()) id = document["catalogId"].as_string();
  if (id.empty()) return catalog_error("catalog_id must be provided or exist in catalog schema.");

  std::string declared;
  if (document["protocolVersion"].is_string()) declared = document["protocolVersion"].as_string();
  std::string requested(protocol_version);
  if (requested.empty()) requested = declared.empty() ? std::string("0.9") : declared;
  const std::string version = normalize_protocol_version(requested);
  if (version.empty()) {
    return catalog_error("Catalog '" + id + "' declares an unsupported protocolVersion '" +
                         requested + "'.");
  }
  const bool v1 = version == "1.0";

  if (v1) {
    std::string pointer;
    A2UI_RETURN_IF_ERROR(check_v10_external_refs(document, pointer, 0));
  }

  const Json& components_in = document["components"];
  if (!components_in.is_undefined() && !components_in.is_object()) {
    return catalog_error("Catalog 'components' must be an object mapping names to schemas.");
  }
  const Json& functions_in = document["functions"];
  if (!functions_in.is_undefined() && !functions_in.is_object() && !functions_in.is_array()) {
    return catalog_error("Catalog '" + id + "' 'functions' must be an object or a list.");
  }
  const Json& defs = document["$defs"];
  const std::vector<std::string> permitted_components =
      catalog_internal::union_member_names(defs["anyComponent"], "#/components/");
  const std::vector<std::string> permitted_functions =
      catalog_internal::union_member_names(defs["anyFunction"], "#/functions/");

  // Validate everything before copying anything.
  for (const auto& m : components_in.as_object()) {
    const std::string& name = m.first;
    if (!m.second.is_object()) {
      return catalog_error("Component '" + name + "' schema must be a JSON schema object.");
    }
    if (!v1) continue;
    if (!is_uax31_identifier(name)) {
      return catalog_error("Invalid UAX #31 component identifier: '" + name + "'");
    }
    if (name == "Surface") {
      return catalog_error("Component name 'Surface' is reserved for the surface container.");
    }
    A2UI_RETURN_IF_ERROR(check_property_identifiers(m.second, name, 0));
    if (const Json* md = m.second.find("metadata")) {
      A2UI_RETURN_IF_ERROR(validate_metadata(*md, "component '" + name + "' metadata"));
    }
  }

  // Functions: the map form, or the v0.9 list form of {name, ...}.
  std::vector<std::pair<std::string, const Json*>> fn_entries;
  if (functions_in.is_object()) {
    for (const auto& m : functions_in.as_object()) {
      if (!m.second.is_object()) {
        return catalog_error("Catalog '" + id + "' function '" + m.first +
                             "' must be a JSON schema object.");
      }
      fn_entries.emplace_back(m.first, &m.second);
    }
  } else if (functions_in.is_array()) {
    for (size_t i = 0; i < functions_in.size(); ++i) {
      const Json& e = functions_in[i];
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
      fn_entries.emplace_back(e["name"].as_string(), &e);
    }
  }
  for (const auto& entry : fn_entries) {
    const std::string& name = entry.first;
    const Json& spec = *entry.second;
    if (!v1) continue;
    if (!is_uax31_identifier(name)) {
      return catalog_error("Invalid UAX #31 function identifier: '" + name + "'" +
                           (name.size() > 0 && name[0] == '@'
                                ? " ('@' names are reserved for protocol system functions)"
                                : ""));
    }
    const Json& props = spec["properties"];
    const Json* args = nullptr;
    if (props["args"].is_object()) {
      args = &props["args"];
    } else if (spec["args"].is_object()) {
      args = &spec["args"];
    } else if (spec["parameters"].is_object()) {
      args = &spec["parameters"];
    }
    std::vector<std::string> params;
    if (args) {
      params = parameter_names(*args, defs);
    } else if (props.is_object() && !props.contains("call") && !props.contains("@call") &&
               !props.contains("function")) {
      params = parameter_names(spec, defs);
    }
    for (const auto& p : params) {
      if (!is_uax31_identifier(p)) {
        return catalog_error("Invalid UAX #31 argument identifier: '" + p + "' in function '" +
                             name + "'");
      }
    }
    const Json& callers = spec["allowedCallers"];
    if (!callers.is_undefined()) {
      const std::string c = callers.is_string() ? callers.as_string() : std::string();
      if (c != "rendererOnly" && c != "agentOnly" && c != "rendererOrAgent") {
        return catalog_error("Function '" + name + "' declares an invalid allowedCallers " +
                             callers.dump() + ".");
      }
      if (spec["requiresUserActivation"].as_bool() && c != "rendererOnly") {
        return catalog_error("Function '" + name +
                             "' declares requiresUserActivation=true with allowedCallers='" + c +
                             "'; requiresUserActivation=true requires allowedCallers='rendererOnly'.");
      }
    }
  }

  // Build. The document is copied once; the loaded definitions are then
  // moved out of the copy.
  auto catalog = std::make_shared<Catalog>();
  catalog->id_ = id;
  catalog->protocol_version_ = version;
  catalog->declared_protocol_version_ = declared.empty() ? std::string(protocol_version) : declared;
  catalog->source_ = document;
  Json& src = catalog->source_;
  const std::shared_ptr<const Json> common = spec::common_types(version);
  const ClassifyCtx ctx{src["$defs"], common.get()};

  if (Json* comps = src.find_mut("components")) {
    Json::Object& members = comps->as_object_mut();
    Json::Object kept;
    for (auto& m : members) {
      if (!permitted_components.empty() && !contains(permitted_components, m.first)) {
        kept.push_back(std::move(m));
        continue;
      }
      ComponentApi api;
      api.name = m.first;
      api.schema = std::move(m.second);
      collect_shape(ctx, api.schema, false, 0, &api);
      if (api.schema["allowedParents"].is_array()) {
        std::vector<std::string> v;
        for (const auto& p : api.schema["allowedParents"].as_array()) {
          if (p.is_string()) v.push_back(p.as_string());
        }
        api.allowed_parents = std::move(v);
      }
      if (api.schema["allowedChildren"].is_array()) {
        std::vector<std::string> v;
        for (const auto& p : api.schema["allowedChildren"].as_array()) {
          if (p.is_string()) v.push_back(p.as_string());
        }
        api.allowed_children = std::move(v);
      }
      if (const Json* md = api.schema.find("metadata")) api.metadata = *md;
      catalog->components_.push_back(std::move(api));
    }
    members = std::move(kept);
  }

  const bool map_form = src["functions"].is_object();
  Json::Object kept_functions;
  for (auto& entry : fn_entries) {
    const std::string& name = entry.first;
    if (!permitted_functions.empty() && !contains(permitted_functions, name)) continue;
    FunctionDecl decl;
    FunctionApi& api = decl.api;
    api.name = name;
    if (map_form) {
      Json* slot = src.find_mut("functions")->find_mut(name);
      // A repeated key leaves an already moved-out slot; copy the entry then.
      if (slot && !slot->is_undefined()) {
        api.schema = std::move(*slot);
      } else {
        api.schema = *entry.second;
      }
    } else {
      api.schema = *entry.second;  // List form (v0.9): kept in the document as written.
      api.schema.erase("name");
    }
    const Json& spec = api.schema;
    api.return_type = declared_return_type(spec);
    if (spec["properties"]["args"].is_object()) {
      api.args_schema = spec["properties"]["args"];
    } else if (spec["args"].is_object()) {
      api.args_schema = spec["args"];
    } else if (spec["parameters"].is_object()) {
      api.args_schema = spec["parameters"];
    }
    const std::string callers = spec["allowedCallers"].is_string() ? spec["allowedCallers"].as_string() : "";
    api.allowed_callers = callers == "agentOnly"         ? AllowedCallers::AgentOnly
                          : callers == "rendererOrAgent" ? AllowedCallers::RendererOrAgent
                                                         : AllowedCallers::RendererOnly;
    api.requires_user_activation = spec["requiresUserActivation"].as_bool();
    if (spec["description"].is_string()) api.description = spec["description"];
    catalog->functions_.push_back(std::move(decl));
  }
  if (map_form) {
    // Drop the moved-out (now undefined) slots; unloaded entries stay.
    Json::Object& members = src.find_mut("functions")->as_object_mut();
    for (auto& m : members) {
      if (!m.second.is_undefined()) kept_functions.push_back(std::move(m));
    }
    members = std::move(kept_functions);
  }
  return catalog;
}

}  // namespace a2ui

// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0

#include "a2ui/catalog.h"

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "a2ui/spec_documents.h"
#include "catalog_internal.h"

namespace a2ui {

const ComponentApi* Catalog::find_component(std::string_view name) const {
  for (const auto& c : components_) {
    if (c.name == name) return &c;
  }
  return nullptr;
}

const FunctionDecl* Catalog::find_function(std::string_view name) const {
  for (const auto& f : functions_) {
    if (f.api.name == name) return &f;
  }
  return nullptr;
}

Status Catalog::bind_function(std::string_view name, FunctionFn impl) {
  for (auto& f : functions_) {
    if (f.api.name == name) {
      f.impl = std::move(impl);
      return ok_status();
    }
  }
  return make_error(ErrorKind::Catalog, "Catalog '" + id_ + "' declares no function '" +
                                            std::string(name) + "'");
}

void Catalog::add_function(FunctionDecl decl) {
  for (auto& f : functions_) {
    if (f.api.name == decl.api.name) {
      f = std::move(decl);
      return;
    }
  }
  functions_.push_back(std::move(decl));
}

void Catalog::add_component(ComponentApi api) {
  if (api.child_refs.empty() && api.schema.is_object()) {
    api.child_refs = catalog_internal::analyze_child_refs(api.schema, source_);
  }
  if (api.schema.is_object() && api.custom_envelope_properties.empty() && !api.checkable) {
    catalog_internal::analyze_envelope(api.schema, source_, &api);
  }
  for (auto& c : components_) {
    if (c.name == api.name) {
      c = std::move(api);
      return;
    }
  }
  components_.push_back(std::move(api));
}

}  // namespace a2ui

// ---------------------------------------------------------------------------
// Serialization, derivation and schema access

namespace a2ui {
namespace {

std::string unescape_pointer_token(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '~' && i + 1 < raw.size() && (raw[i + 1] == '0' || raw[i + 1] == '1')) {
      out.push_back(raw[i + 1] == '0' ? '~' : '/');
      ++i;
    } else {
      out.push_back(raw[i]);
    }
  }
  return out;
}

// Splits "/<head>/<name>[/rest]" when <head> is `head`.
bool split_entry_pointer(std::string_view pointer, std::string_view head, std::string* name,
                         std::string_view* rest) {
  if (pointer.size() < head.size() + 2 || pointer[0] != '/' ||
      pointer.substr(1, head.size()) != head || pointer[head.size() + 1] != '/') {
    return false;
  }
  std::string_view tail = pointer.substr(head.size() + 2);
  const size_t slash = tail.find('/');
  *name = unescape_pointer_token(tail.substr(0, slash));
  *rest = slash == std::string_view::npos ? std::string_view() : tail.substr(slash);
  return true;
}

Json code_function_definition(const FunctionApi& api) {
  Json props = Json::object({{"@call", Json::object({{"const", Json(api.name)}})}});
  props.set("args", api.args_schema.is_object() ? api.args_schema : Json::object({{"type", Json("object")}}));
  Json def = Json::object({{"type", Json("object")}});
  if (api.description.is_string()) def.set("description", api.description);
  def.set("returnType", Json(api.return_type));
  if (api.allowed_callers != AllowedCallers::RendererOnly) {
    def.set("allowedCallers",
            Json(api.allowed_callers == AllowedCallers::AgentOnly ? "agentOnly" : "rendererOrAgent"));
  }
  if (api.requires_user_activation) def.set("requiresUserActivation", Json(true));
  def.set("properties", std::move(props));
  def.set("required", Json::array({Json("@call")}));
  return def;
}

}  // namespace

const Json* Catalog::resolve_catalog_pointer(std::string_view pointer) const {
  std::string name;
  std::string_view rest;
  if (split_entry_pointer(pointer, "components", &name, &rest)) {
    if (const ComponentApi* c = find_component(name)) {
      return c->schema.is_undefined() ? nullptr : resolve_json_pointer(c->schema, rest);
    }
  } else if (split_entry_pointer(pointer, "functions", &name, &rest)) {
    if (const FunctionDecl* f = find_function(name); f && !f->api.schema.is_undefined()) {
      return resolve_json_pointer(f->api.schema, rest);
    }
  }
  if (const Json* j = resolve_json_pointer(source_, pointer)) return j;
  // A local "#/$defs/X" the catalog does not define names the common type X
  // (the other SDKs bundle common types into the catalog's $defs).
  if (pointer.rfind("/$defs/", 0) == 0) {
    if (auto common = spec::common_types(protocol_version_)) return resolve_json_pointer(*common, pointer);
  }
  return nullptr;
}

SchemaRegistry Catalog::schema_registry() const {
  SchemaRegistry reg;
  if (auto common = spec::common_types(protocol_version_)) reg.add("common_types.json", common);
  reg.add("catalog.json", nullptr,
          [this](std::string_view pointer) { return resolve_catalog_pointer(pointer); });
  return reg;
}

Json Catalog::to_json() const {
  Json out = source_.is_object() ? source_ : Json::object();
  if (!source_.is_object()) {
    out.set("catalogId", Json(id_));
    out.set("protocolVersion", Json(protocol_version_));
  }
  if (!components_.empty() || out.contains("components")) {
    Json comps = out["components"].is_object() ? out["components"] : Json::object();
    for (const auto& c : components_) {
      comps.set(c.name, c.schema.is_undefined() ? Json::object({{"type", Json("object")}}) : c.schema);
    }
    out.set("components", std::move(comps));
  }
  if (!out["functions"].is_array()) {
    bool any = out.contains("functions");
    Json fns = out["functions"].is_object() ? out["functions"] : Json::object();
    for (const auto& f : functions_) {
      if (!f.api.name.empty() && f.api.name[0] == '@') continue;  // System functions.
      fns.set(f.api.name, f.api.schema.is_undefined() ? code_function_definition(f.api) : f.api.schema);
      any = true;
    }
    if (any) out.set("functions", std::move(fns));
  }
  return out;
}

Result<CatalogPtr> Catalog::derive(std::string_view id, const std::vector<DeriveEntry>* components,
                                   const std::vector<DeriveEntry>* functions,
                                   const std::string* instructions) const {
  auto d = make_ref<Catalog>();
  d->id_ = std::string(id);
  d->protocol_version_ = protocol_version_;
  d->declared_protocol_version_ = declared_protocol_version_;

  // New entries are loaded through from_json so they get the same checks.
  auto load_new = [&](const char* kind, const DeriveEntry& e) -> Result<CatalogPtr> {
    Json holder = Json::object({{"catalogId", Json("derive-entries")},
                                {"protocolVersion", Json(protocol_version_)}});
    holder.set(kind, Json::object({{e.name, e.definition}}));
    return Catalog::from_json(holder);
  };

  if (components) {
    for (const auto& e : *components) {
      if (e.definition.is_undefined()) {
        const ComponentApi* c = find_component(e.name);
        if (!c) return make_error(ErrorKind::Catalog, "Catalog \"" + id_ + "\" has no component \"" + e.name + "\".");
        d->add_component(*c);
      } else {
        auto r = load_new("components", e);
        if (!r.ok()) return r.take_error();
        for (const auto& c : r.value()->components()) d->add_component(c);
      }
    }
  } else {
    d->components_ = components_;
  }
  if (functions) {
    for (const auto& e : *functions) {
      if (e.definition.is_undefined()) {
        const FunctionDecl* f = find_function(e.name);
        if (!f) return make_error(ErrorKind::Catalog, "Catalog \"" + id_ + "\" has no function \"" + e.name + "\".");
        d->add_function(*f);
      } else {
        auto r = load_new("functions", e);
        if (!r.ok()) return r.take_error();
        for (const auto& f : r.value()->functions()) d->add_function(f);
      }
    }
  } else {
    d->functions_ = functions_;
  }

  Json src = source_.is_object() ? source_ : Json::object();
  for (const char* k : {"$id", "title", "description", "instructions"}) src.erase(k);
  src.set("catalogId", Json(d->id_));
  if (instructions) src.set("instructions", Json(*instructions));
  if (src.contains("components") || !d->components_.empty()) src.set("components", Json::object());
  if (src.contains("functions") || !d->functions_.empty()) src.set("functions", Json::object());
  if (Json* defs = src.find_mut("$defs"); defs && defs->is_object()) {
    if (defs->contains("anyComponent")) {
      std::vector<std::string> names;
      for (const auto& c : d->components_) names.push_back(c.name);
      defs->set("anyComponent", catalog_internal::make_union(names, "#/components/", true));
    }
    if (defs->contains("anyFunction")) {
      std::vector<std::string> names;
      for (const auto& f : d->functions_) {
        if (f.api.name.empty() || f.api.name[0] != '@') names.push_back(f.api.name);
      }
      defs->set("anyFunction", catalog_internal::make_union(names, "#/functions/", false));
    }
  }
  d->source_ = std::move(src);
  return d;
}

}  // namespace a2ui

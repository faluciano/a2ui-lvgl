// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// ComponentModel and the child reference map. See a2ui/component_model.h.

#include "a2ui/component_model.h"

#include <utility>

#include "a2ui/metadata.h"
#include "a2ui/schema.h"

namespace a2ui {
namespace {

void append_segment(std::string* path, std::string_view seg) {
  *path += '/';
  *path += escape_pointer_token(seg);
}

// extract_child_refs_from_val of the Python reference map.
void refs_from_value(const Json& val, const std::string& base, std::vector<ChildRef>* out, int depth) {
  if (depth > 16) return;
  if (val.is_string()) {
    if (!val.as_string().empty()) out->push_back({val.as_string(), base});
  } else if (val.is_array()) {
    for (size_t i = 0; i < val.size(); ++i) {
      refs_from_value(val[i], base + "/" + std::to_string(i), out, depth + 1);
    }
  } else if (val.is_object()) {
    const Json& cid = val["componentId"];
    if (cid.is_string() && !cid.as_string().empty()) {
      out->push_back({cid.as_string(), base + "/componentId"});
      return;
    }
    const Json& child = val["child"];
    if (child.is_string() && !child.as_string().empty()) {
      out->push_back({child.as_string(), base + "/child"});
      return;
    }
    for (const auto& m : val.as_object()) {
      std::string p = base;
      append_segment(&p, m.first);
      refs_from_value(m.second, p, out, depth + 1);
    }
  }
}

// Naming conventions of the legacy reference heuristic, used for component
// types the catalog does not declare.
bool is_heuristic_child_key(const std::string& key) {
  static const char* const kSingle[] = {"child", "contentChild", "entryPointChild", "componentId"};
  static const char* const kList[] = {"children", "explicitList", "template", "tabs"};
  for (const char* k : kSingle) {
    if (key == k) return true;
  }
  for (const char* k : kList) {
    if (key == k) return true;
  }
  auto ends = [&](std::string_view s) {
    return key.size() >= s.size() && key.compare(key.size() - s.size(), s.size(), s) == 0;
  };
  return ends("Child") || ends("children") || key.rfind("child", 0) == 0;
}

}  // namespace

void child_references_of(const ComponentApi* api, const Json& props, std::vector<ChildRef>* out) {
  if (!props.is_object()) return;
  if (!api || api->child_refs.empty()) {
    if (api && api->schema.is_object()) return;  // A declared type with no reference props.
    for (const auto& m : props.as_object()) {
      if (!is_heuristic_child_key(m.first)) continue;
      std::string base;
      append_segment(&base, m.first);
      refs_from_value(m.second, base, out, 0);
    }
    return;
  }
  const ComponentApi::ChildRefs& refs = api->child_refs;
  for (const auto& key : refs.single) {
    const Json* v = props.find(key);
    if (!v) continue;
    std::string base;
    append_segment(&base, key);
    refs_from_value(*v, base, out, 0);
  }
  for (const auto& key : refs.list) {
    const Json* v = props.find(key);
    if (!v) continue;
    std::string base;
    append_segment(&base, key);
    if (v->is_array()) {
      auto nested_it = refs.nested.find(key);
      for (size_t i = 0; i < v->size(); ++i) {
        const Json& item = (*v)[i];
        const std::string item_path = base + "/" + std::to_string(i);
        if (item.is_string()) {
          if (!item.as_string().empty()) out->push_back({item.as_string(), item_path});
        } else if (item.is_object()) {
          const Json& cid = item["componentId"];
          if (cid.is_string() && !cid.as_string().empty()) {
            out->push_back({cid.as_string(), item_path + "/componentId"});
          } else if (nested_it != refs.nested.end()) {
            for (const auto& sub : nested_it->second) {
              if (const Json* sv = item.find(sub)) {
                std::string p = item_path;
                append_segment(&p, sub);
                refs_from_value(*sv, p, out, 0);
              }
            }
          } else {
            refs_from_value(item, item_path, out, 0);
          }
        }
      }
    } else if (v->is_string()) {
      if (!v->as_string().empty()) out->push_back({v->as_string(), base});
    } else if (v->is_object()) {
      const Json& cid = (*v)["componentId"];
      if (cid.is_string() && !cid.as_string().empty()) {
        out->push_back({cid.as_string(), base + "/componentId"});
      } else {
        refs_from_value(*v, base, out, 0);
      }
    }
  }
}

ComponentModel::ComponentModel(std::string id, std::string type, CatalogPtr catalog, Json properties,
                               Json metadata)
    : id_(std::move(id)),
      type_(std::move(type)),
      catalog_(std::move(catalog)),
      api_(catalog_ ? catalog_->find_component(type_) : nullptr),
      properties_(properties.is_object() ? std::move(properties) : Json::object()),
      metadata_(normalize_metadata(metadata)) {}

void ComponentModel::set_properties(Json properties) {
  if (!properties.is_object()) properties = Json::object();
  if (properties == properties_) return;
  properties_ = std::move(properties);
  if (owner_revision_) ++*owner_revision_;
  on_updated_.emit(this);
}

Status ComponentModel::set_metadata(const Json& metadata) {
  Json next;
  auto changed = metadata_prepare(metadata_, metadata, "component '" + id_ + "' metadata", &next);
  if (!changed.ok()) return changed.take_error();
  if (!changed.value()) return ok_status();
  metadata_ = std::move(next);
  on_metadata_changed_.emit(metadata_);
  return ok_status();
}

Status ComponentModel::set_extension(const std::string& key, Json value) {
  auto next = metadata_with_extension(metadata_, key, std::move(value), "component '" + id_ + "' metadata");
  if (!next.ok()) return next.take_error();
  return set_metadata(next.value());
}

void ComponentModel::remove_extension(const std::string& key) {
  (void)set_metadata(metadata_without_extension(metadata_, key));
}

void ComponentModel::apply_update(Json properties, Json metadata) {
  Batch batch;
  (void)set_metadata(metadata);
  set_properties(std::move(properties));
}

Json ComponentModel::component_tree() const {
  Json out = Json::object({{"id", Json(id_)}, {"component", Json(type_)}});
  for (const auto& m : properties_.as_object()) out.set(m.first, m.second);
  if (!metadata_.is_undefined()) out.set("@metadata", metadata_);
  return out;
}

void ComponentModel::child_references(std::vector<ChildRef>* out) const {
  child_references_of(api_, properties_, out);
}

void ComponentModel::dispose() {
  on_updated_.clear();
  on_metadata_changed_.clear();
  owner_revision_ = nullptr;
}

}  // namespace a2ui

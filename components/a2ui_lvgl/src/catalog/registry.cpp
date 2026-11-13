// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0

#include "a2ui_lvgl/component.h"

namespace a2ui_lvgl {

void ComponentRegistry::add(ComponentImplementation impl) {
  for (auto& e : entries_) {
    if (e.name == impl.name && e.catalog_id == impl.catalog_id) {
      e = std::move(impl);
      return;
    }
  }
  entries_.push_back(std::move(impl));
}

const ComponentImplementation* ComponentRegistry::find(std::string_view catalog_id, std::string_view name) const {
  const ComponentImplementation* any = nullptr;
  for (const auto& e : entries_) {
    if (e.name != name || !e.create) continue;
    if (!catalog_id.empty() && e.catalog_id == catalog_id) return &e;
    if (e.catalog_id.empty()) any = &e;
  }
  return any;
}

}  // namespace a2ui_lvgl

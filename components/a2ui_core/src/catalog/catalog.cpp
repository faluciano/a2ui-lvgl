// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0

#include "a2ui/catalog.h"

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
  for (auto& c : components_) {
    if (c.name == api.name) {
      c = std::move(api);
      return;
    }
  }
  components_.push_back(std::move(api));
}

}  // namespace a2ui

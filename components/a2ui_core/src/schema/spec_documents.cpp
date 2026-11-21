// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0

#include "a2ui/spec_documents.h"

#include "spec_documents_data.h"

namespace a2ui {
namespace spec {
namespace {

std::shared_ptr<const Json> parse_text(std::string_view text) {
  auto r = Json::parse(text, 64);
  return std::make_shared<const Json>(r.ok ? std::move(r.value) : Json::object());
}

}  // namespace

std::string_view text(Document doc) {
  switch (doc) {
    case Document::CommonTypesV1_0:
      return {spec_data::kCommonTypesV1_0, spec_data::kCommonTypesV1_0Size};
    case Document::AgentToRendererV1_0:
      return {spec_data::kAgentToRendererV1_0, spec_data::kAgentToRendererV1_0Size};
    case Document::BasicCatalogV1:
      return {spec_data::kBasicCatalogV1, spec_data::kBasicCatalogV1Size};
  }
  return {};
}

std::shared_ptr<const Json> parsed(Document doc) {
  switch (doc) {
    case Document::CommonTypesV1_0: {
      static const std::shared_ptr<const Json> d = parse_text(text(doc));
      return d;
    }
    case Document::AgentToRendererV1_0: {
      static const std::shared_ptr<const Json> d = parse_text(text(doc));
      return d;
    }
    case Document::BasicCatalogV1: {
      static const std::shared_ptr<const Json> d = parse_text(text(doc));
      return d;
    }
  }
  return parse_text("{}");
}

std::shared_ptr<const Json> common_types(std::string_view protocol_version) {
  if (protocol_version == "1.0") return parsed(Document::CommonTypesV1_0);
  return nullptr;
}

}  // namespace spec
}  // namespace a2ui

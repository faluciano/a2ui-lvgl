// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0

#include "a2ui/spec_documents.h"

#include "spec_documents_data.h"

namespace a2ui {
namespace spec {
namespace {

Ref<const Json> parse_text(std::string_view text) {
  auto r = Json::parse(text, 64);
  return make_ref<const Json>(r.ok ? std::move(r.value) : Json::object());
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

Ref<const Json> parsed(Document doc) {
  switch (doc) {
    case Document::CommonTypesV1_0: {
      static const Ref<const Json> d = parse_text(text(doc));
      return d;
    }
    case Document::AgentToRendererV1_0: {
      static const Ref<const Json> d = parse_text(text(doc));
      return d;
    }
    case Document::BasicCatalogV1: {
      static const Ref<const Json> d = parse_text(text(doc));
      return d;
    }
  }
  return parse_text("{}");
}

Ref<const Json> common_types(std::string_view protocol_version) {
  if (protocol_version == "1.0") return parsed(Document::CommonTypesV1_0);
  return nullptr;
}

const PackedDocument* packed(Document doc) {
  switch (doc) {
    case Document::CommonTypesV1_0: return &spec_data::kCommonTypesV1_0Packed;
    case Document::AgentToRendererV1_0: return &spec_data::kAgentToRendererV1_0Packed;
    case Document::BasicCatalogV1: return &spec_data::kBasicCatalogV1Packed;
  }
  return &spec_data::kCommonTypesV1_0Packed;
}

const PackedDocument* packed_common_types(std::string_view protocol_version) {
  if (protocol_version == "1.0") return packed(Document::CommonTypesV1_0);
  return nullptr;
}

}  // namespace spec
}  // namespace a2ui

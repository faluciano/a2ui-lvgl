// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Private: the embedded specification documents, as text
// (spec_documents_data.cpp) and as packed tables (packed_documents_data.cpp).

#pragma once

#include <cstddef>
#include <cstdint>

#include "a2ui/schema_node.h"

namespace a2ui {
namespace spec_data {

extern const char kCommonTypesV1_0[];
extern const size_t kCommonTypesV1_0Size;
extern const char kAgentToRendererV1_0[];
extern const size_t kAgentToRendererV1_0Size;
extern const char kBasicCatalogV1[];
extern const size_t kBasicCatalogV1Size;

extern const PackedDocument kCommonTypesV1_0Packed;
extern const PackedDocument kAgentToRendererV1_0Packed;
extern const PackedDocument kBasicCatalogV1Packed;

}  // namespace spec_data
}  // namespace a2ui

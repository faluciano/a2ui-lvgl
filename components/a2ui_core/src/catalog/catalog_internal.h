// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Private helpers shared by the catalog sources.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "a2ui/json.h"

namespace a2ui {
namespace catalog_internal {

/**
 * True if `ref` points into common_types.json's $defs (relative or absolute
 * URI). Sets the definition name and the pointer remainder after it.
 */
bool is_common_types_ref(std::string_view ref, std::string* def_name, std::string* rest);

/** Entry names a `oneOf` union lists as `<prefix><name>` references. */
std::vector<std::string> union_member_names(const Json& union_schema, std::string_view prefix);

/** {"oneOf": [{"$ref": "<prefix><name>"}...]}, plus the component discriminator if asked. */
Json make_union(const std::vector<std::string>& names, std::string_view prefix, bool discriminator);

}  // namespace catalog_internal
}  // namespace a2ui

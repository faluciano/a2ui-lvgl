// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Private helpers shared by the catalog sources.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "a2ui/catalog.h"
#include "a2ui/json.h"
#include "a2ui/schema_node.h"

namespace a2ui {
namespace catalog_internal {

/**
 * True if `ref` points into common_types.json's $defs (relative or absolute
 * URI). Sets the definition name and the pointer remainder after it.
 */
bool is_common_types_ref(std::string_view ref, std::string* def_name, std::string* rest);

/** Entry names a `oneOf` union lists as `<prefix><name>` references. */
std::vector<std::string> union_member_names(SchemaNode union_schema, std::string_view prefix);

/** {"oneOf": [{"$ref": "<prefix><name>"}...]}, plus the component discriminator if asked. */
Json make_union(const std::vector<std::string>& names, std::string_view prefix, bool discriminator);

/**
 * Finds the child reference properties of a component schema (the Python
 * SDK's analyze_child_ref_schema): `$ref`s ending in /ComponentId or /Child
 * are single references, /ChildList (or an array of either) a list, and an
 * array of objects with such properties a nested list. Local "#/..." refs
 * resolve against the schema's own `$defs`, then `catalog_doc`.
 */
ComponentApi::ChildRefs analyze_child_refs(SchemaNode schema, SchemaNode catalog_doc);

/** Fills custom_envelope_properties and checkable (the Python SDK's get_custom_envelope_properties). */
void analyze_envelope(SchemaNode schema, SchemaNode catalog_doc, ComponentApi* api);

}  // namespace catalog_internal
}  // namespace a2ui

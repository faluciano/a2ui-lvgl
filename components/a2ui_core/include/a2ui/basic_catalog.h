// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// The v1.0 Basic Catalog's renderer-side functions (catalogs/basic/v1).

#pragma once

#include <string>

#include "a2ui/catalog.h"

namespace a2ui {
namespace basic_catalog {

constexpr const char* kCatalogIdV1 = "https://a2ui.org/specification/v1_0/catalogs/basic/catalog.json";

/** Looks up the implementation of a basic catalog function by name (empty if unknown). */
FunctionFn function_impl(const std::string& name);

/** Binds every basic function implementation to the matching declarations in `catalog`. */
void bind_functions(Catalog& catalog);

/**
 * Media URL validation shared by Image, Video and AudioPlayer
 * (basic catalog guide, "Media URLs"). Returns false when the URL must not load.
 */
bool is_allowed_media_url(const std::string& url);

/** openUrl scheme check: only absolute http(s) URLs. */
bool is_allowed_open_url(const std::string& url);

}  // namespace basic_catalog
}  // namespace a2ui

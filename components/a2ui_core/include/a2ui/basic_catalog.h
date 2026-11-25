// Copyright 2026 The a2ui-lvgl Authors
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
 * Loads the v1.0 basic catalog and binds its functions.
 *
 * Compact: Catalog::from_packed on the packed catalog in flash. The schemas
 * are read in place and only the component and function summaries use the
 * heap (about 15 KB on an ESP32). For chips without PSRAM.
 *
 * Not compact: Catalog::from_json on a freshly parsed copy of the catalog,
 * which the catalog keeps (about 100 KB on an ESP32, plus the parsed common
 * types). Catalog::to_json(), source() and ComponentApi::schema are Json.
 *
 * Validation gives the same results either way.
 */
Result<CatalogPtr> load(bool compact);

/**
 * load() with the build's choice: compact when CONFIG_A2UI_COMPACT_CATALOG
 * is set (ESP-IDF, menuconfig "A2UI core"), with the CMake option
 * A2UI_COMPACT_CATALOG, or when a2ui_core is compiled with
 * -DA2UI_COMPACT_CATALOG=1.
 */
Result<CatalogPtr> load();

/** True when load() loads the compact catalog. */
bool compact_by_default();

/**
 * Media URL validation shared by Image, Video and AudioPlayer
 * (basic catalog guide, "Media URLs"). Returns false when the URL must not load.
 */
bool is_allowed_media_url(const std::string& url);

/** openUrl scheme check: only absolute http(s) URLs. */
bool is_allowed_open_url(const std::string& url);

}  // namespace basic_catalog
}  // namespace a2ui

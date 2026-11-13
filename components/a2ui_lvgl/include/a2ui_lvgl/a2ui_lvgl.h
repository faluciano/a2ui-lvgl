// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// LVGL framework adapter for the A2UI core.
//
// PLACEHOLDER: this header only exists so the component chain
// (a2ui_core -> a2ui_lvgl -> lvgl -> app) compiles and links on host and
// device. The real adapter replaces it.

#pragma once

#include <string>
#include <vector>

#include "a2ui/json.h"
#include "lvgl.h"

namespace a2ui::lvgl {

/**
 * Collects the `text` property of every component in the `updateComponents`
 * messages of `messages`. Accepts either a JSON array of A2UI messages or an
 * object with a `messages` array (the upstream example file format).
 */
std::vector<std::string> collect_component_text(const Json& messages);

/**
 * Creates a full-size vertical flex container under `parent` with one label
 * per entry in `lines`. Returns the container. Call with the LVGL lock held.
 */
lv_obj_t* create_text_column(lv_obj_t* parent, const std::vector<std::string>& lines);

/** collect_component_text() followed by create_text_column(). */
lv_obj_t* render_placeholder(lv_obj_t* parent, const Json& messages);

}  // namespace a2ui::lvgl

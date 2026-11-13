// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// PLACEHOLDER adapter; see a2ui_lvgl.h.

#include "a2ui_lvgl/a2ui_lvgl.h"

namespace a2ui::lvgl {

std::vector<std::string> collect_component_text(const Json& messages) {
  const Json* list = &messages;
  if (messages.is_object()) list = messages.find("messages");
  std::vector<std::string> out;
  if (list == nullptr || !list->is_array()) return out;
  for (const Json& msg : list->as_array()) {
    const Json* update = msg.find("updateComponents");
    if (update == nullptr) continue;
    const Json* components = update->find("components");
    if (components == nullptr || !components->is_array()) continue;
    for (const Json& component : components->as_array()) {
      const Json* text = component.find("text");
      if (text != nullptr && text->is_string()) out.push_back(text->as_string());
    }
  }
  return out;
}

lv_obj_t* create_text_column(lv_obj_t* parent, const std::vector<std::string>& lines) {
  lv_obj_t* column = lv_obj_create(parent);
  lv_obj_set_size(column, lv_pct(100), lv_pct(100));
  lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(column, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  for (const std::string& line : lines) {
    lv_obj_t* label = lv_label_create(column);
    lv_label_set_text(label, line.c_str());
  }
  return column;
}

lv_obj_t* render_placeholder(lv_obj_t* parent, const Json& messages) {
  return create_text_column(parent, collect_component_text(messages));
}

}  // namespace a2ui::lvgl

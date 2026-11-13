// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0

#include "a2ui_lvgl/basic_catalog.h"
#include "basic_components.h"

namespace a2ui_lvgl {

void register_basic_components(ComponentRegistry& registry) {
  using Factory = std::unique_ptr<ComponentInstance> (*)();
  static const struct {
    const char* name;
    Factory make;
  } kEntries[] = {
      {"Text", &basic::make_text},
      {"Image", &basic::make_image},
      {"Icon", &basic::make_icon},
      {"Video", &basic::make_video},
      {"AudioPlayer", &basic::make_audio_player},
      {"Row", &basic::make_row},
      {"Column", &basic::make_column},
      {"List", &basic::make_list},
      {"Card", &basic::make_card},
      {"Tabs", &basic::make_tabs},
      {"Modal", &basic::make_modal},
      {"Divider", &basic::make_divider},
      {"Button", &basic::make_button},
      {"TextField", &basic::make_text_field},
      {"CheckBox", &basic::make_check_box},
      {"ChoicePicker", &basic::make_choice_picker},
      {"Slider", &basic::make_slider},
      {"DateTimeInput", &basic::make_date_time_input},
  };
  for (const auto& e : kEntries) {
    ComponentImplementation impl;
    impl.name = e.name;
    impl.create = e.make;
    registry.add(std::move(impl));
  }
}

const ComponentRegistry& basic_registry() {
  static const ComponentRegistry registry = [] {
    ComponentRegistry r;
    register_basic_components(r);
    return r;
  }();
  return registry;
}

}  // namespace a2ui_lvgl

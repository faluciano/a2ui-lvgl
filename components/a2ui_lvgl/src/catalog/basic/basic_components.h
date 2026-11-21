// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Factories of the basic catalog component instances.

#pragma once

#include <memory>

#include "a2ui_lvgl/component.h"
#include "basic_common.h"

namespace a2ui_lvgl {
namespace basic {

std::unique_ptr<ComponentInstance> make_text();
std::unique_ptr<ComponentInstance> make_icon();
std::unique_ptr<ComponentInstance> make_image();
std::unique_ptr<ComponentInstance> make_video();
std::unique_ptr<ComponentInstance> make_audio_player();
std::unique_ptr<ComponentInstance> make_row();
std::unique_ptr<ComponentInstance> make_column();
std::unique_ptr<ComponentInstance> make_list();
std::unique_ptr<ComponentInstance> make_card();
std::unique_ptr<ComponentInstance> make_tabs();
std::unique_ptr<ComponentInstance> make_modal();
std::unique_ptr<ComponentInstance> make_divider();
std::unique_ptr<ComponentInstance> make_button();
std::unique_ptr<ComponentInstance> make_text_field();
std::unique_ptr<ComponentInstance> make_check_box();
std::unique_ptr<ComponentInstance> make_choice_picker();
std::unique_ptr<ComponentInstance> make_slider();
std::unique_ptr<ComponentInstance> make_date_time_input();

}  // namespace basic
}  // namespace a2ui_lvgl

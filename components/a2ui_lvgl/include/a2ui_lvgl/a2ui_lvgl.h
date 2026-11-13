// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// LVGL framework adapter for the A2UI core: umbrella header.
//
//   a2ui::MessageProcessor processor({catalog});
//   processor.process_text(json);
//   a2ui_lvgl::SurfaceConfig config;
//   config.registry = &a2ui_lvgl::basic_registry();
//   a2ui_lvgl::Surface view(*processor.model().get_surface("s"), lv_screen_active(), config);
//
// Layout of the component:
//   surface.h        Surface host, fallbacks, services, config
//   component.h      ComponentInstance (Form 4), registry, NodeView dispatcher
//   theme.h          colours, fonts, spacing (v1.0 has no protocol theme)
//   accessibility.h  accessibility seam (LVGL has no accessibility API)
//   image_loader.h   application-provided image fetching
//   markdown.h       the Markdown subset Text renders
//   basic_catalog.h  basic catalog widgets

#pragma once

#include "a2ui_lvgl/accessibility.h"
#include "a2ui_lvgl/basic_catalog.h"
#include "a2ui_lvgl/component.h"
#include "a2ui_lvgl/image_loader.h"
#include "a2ui_lvgl/markdown.h"
#include "a2ui_lvgl/surface.h"
#include "a2ui_lvgl/theme.h"

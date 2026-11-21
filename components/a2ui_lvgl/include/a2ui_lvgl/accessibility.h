// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Accessibility seam. LVGL has no accessibility API, so the adapter keeps
// the resolved A2UI accessibility attributes with each view and exposes
// them here: a screen-reader bridge, a test, or a speech/haptics layer on
// the device reads them from any rendered lv_obj_t.
//
// The attributes are computed in one place (NodeView::accessibility) for
// every component, custom ones included:
//   1. `@accessibility` (label, description, live, hidden) as resolved by
//      the core, the legacy bare `accessibility` key already normalized to
//      it on v1.0 surfaces;
//   2. otherwise inferred from visible content by the implementation
//      (ComponentInstance::infer_accessibility): a Button's child text, a
//      field's label, a Text's text. An explicit value always wins.

#pragma once

#include <optional>
#include <string>

#include "lvgl.h"

namespace a2ui_lvgl {

class NodeView;

struct Accessibility {
  std::string label;
  std::string description;
  /** "off", "polite" or "assertive"; empty when not a live region. */
  std::string live;
  bool hidden = false;
  /** ARIA-style role: "button", "textbox", "checkbox", "radiogroup", "group", "slider", "tablist", "img", "text", "dialog", ... */
  std::string role;
  /** Checked state of a checkbox, radio or toggle. */
  std::optional<bool> checked;
  /** Whether the control currently refuses input (invalid Button, ...). */
  bool disabled = false;
};

/** The view rendering `obj` (its root object), or null. */
NodeView* view_of(lv_obj_t* obj);

/** The nearest view whose root object is `obj` or one of its ancestors, or null. */
NodeView* enclosing_view(lv_obj_t* obj);

/** Accessibility attributes of the view rendering `obj` (see enclosing_view); nullopt for foreign objects. */
std::optional<Accessibility> accessibility_of(lv_obj_t* obj);

}  // namespace a2ui_lvgl

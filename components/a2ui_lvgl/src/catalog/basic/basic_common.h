// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Shared pieces of the basic catalog widgets: a per-surface style sheet built
// from the Theme, container/leaf helpers, and the flex child layout used by
// Row, Column, List and Card.

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "a2ui/json.h"
#include "a2ui_lvgl/component.h"
#include "a2ui_lvgl/surface.h"
#include "a2ui_lvgl/theme.h"
#include "lvgl.h"

namespace a2ui_lvgl {
namespace basic {

/** Shared LVGL styles for one surface (lv_style_t objects must outlive the objects using them). */
struct Styles {
  explicit Styles(const Theme& theme);
  ~Styles();
  Styles(const Styles&) = delete;
  Styles& operator=(const Styles&) = delete;

  lv_style_t container;        // Invisible structural container: no padding, gap, border or background.
  lv_style_t leaf;             // Uniform external margin.
  lv_style_t card;             // Transparent, outlined, rounded, padded.
  lv_style_t button;           // Shape shared by every button variant.
  lv_style_t button_default;   // Subtle background and outline.
  lv_style_t button_primary;   // Primary background, contrasting text.
  lv_style_t button_borderless;
  lv_style_t pressed;          // Darkens on press.
  lv_style_t disabled;         // Faded while disabled.
  lv_style_t caption;          // Smaller, muted text.
  lv_style_t error_text;
  lv_style_t warning_text;
  lv_style_t info_text;
  lv_style_t divider;
  lv_style_t field;            // Text area / picker field.
  lv_style_t field_focused;
  lv_style_t chip;
  lv_style_t chip_checked;
  lv_style_t tab;
  lv_style_t tab_active;
  lv_style_t placeholder_box;  // Image placeholder.
  lv_style_t scrim;            // Modal / popup backdrop.
  lv_style_t dialog;           // Modal / popup panel.
  lv_style_t radio_indicator;  // Round checkbox indicator for exclusive choices.

  const Theme& theme;
};

/** The surface's style sheet (created on first use). */
Styles& styles(NodeView& view);
Styles& styles(Surface& surface);

/** A plain object with the container style (no theme styling), not clickable or scrollable. */
lv_obj_t* make_container(lv_obj_t* parent, const Styles& s);

/** Sets a label's text only if it differs (avoids relayout). */
void set_label_text(lv_obj_t* label, const std::string& text);
/** Width/height setters that skip no-op writes. */
bool set_width(lv_obj_t* obj, int32_t w);
void set_height(lv_obj_t* obj, int32_t h);

enum class Axis { Row, Column };

/**
 * Lays out the child views of a flex container `container`:
 *   - `weight` -> flex grow on the main axis; justify == "stretch" grows
 *     every flexible child;
 *   - Column, align == "stretch": flexible children take the full width
 *     (content width when the container itself is content-sized);
 *   - Row: full-width children (Column, Card, fields) share the free space
 *     (grow 1); content-sized children (Text, Button) keep their content
 *     width, capped at the row's width (the Row narrows overflowing text
 *     after layout, see layout.cpp).
 * Fixed-size children (an icon, a small image) keep their size.
 */
void layout_children(lv_obj_t* container, const std::vector<NodeView*>& kids, Axis axis, std::string_view align,
                     std::string_view justify);

/** Makes full-width children content-sized (inside a content-sized parent such as a Button). */
void fit_to_content(const std::vector<NodeView*>& kids);

/** Maps a basic catalog `justify` value onto flex main-axis placement. */
lv_flex_align_t main_place(std::string_view justify);
/** Maps a basic catalog `align` value onto flex cross-axis placement. */
lv_flex_align_t cross_place(std::string_view align);

/** The visible text of a view's subtree (Text/Icon labels), for implicit accessibility labels. */
std::string subtree_text(const NodeView& view);

/** Splits on '\n' (drops empty lines). */
std::vector<std::string> split_lines(const std::string& s);
std::string join(const std::vector<std::string>& parts, std::string_view sep);

/**
 * Validation feedback of a checkable control: blocking error messages
 * (validationErrors) and non-blocking hints (messages of failed warning and
 * info results), shown under the control in one label.
 */
struct CheckFeedback {
  std::vector<std::string> errors;
  std::vector<std::string> hints;
  std::string hint_severity;  // "warning" or "info" (the most severe hint).
};
CheckFeedback check_feedback(const NodeView& view);

/** Shows `fb` in `label` (hidden when empty) styled by severity. */
void show_feedback(lv_obj_t* label, const CheckFeedback& fb, const Styles& s);

/**
 * Reads the label back: {"validationErrors": [...]} or {"hints": [...]}
 * depending on what it shows (for describe()).
 */
void describe_feedback(lv_obj_t* label, a2ui::Json* out);

/** Marks the start of an LVGL event handler that calls into the core. */
bool is_disabled(lv_obj_t* obj);

}  // namespace basic
}  // namespace a2ui_lvgl

namespace a2ui_lvgl {
namespace basic {

/**
 * A dimmed full-screen backdrop on the display's top layer with a centered
 * panel: Modal's dialog and DateTimeInput's picker. Tapping the backdrop
 * calls `on_dismiss`. The objects are owned here and deleted by close();
 * deleting them from outside is noticed (is_open() turns false).
 */
class Popup {
 public:
  Popup() = default;
  ~Popup() { close(); }
  Popup(const Popup&) = delete;
  Popup& operator=(const Popup&) = delete;

  /** Opens on the top layer of `anchor`'s display; returns the panel (a flex column). */
  lv_obj_t* open(lv_obj_t* anchor, const Styles& s, void (*on_dismiss)(void* user), void* user);
  void close();
  bool is_open() const { return overlay_ != nullptr; }
  lv_obj_t* panel() const { return panel_; }

 private:
  static void on_event(lv_event_t* e);
  lv_obj_t* overlay_ = nullptr;
  lv_obj_t* panel_ = nullptr;
  void (*on_dismiss_)(void*) = nullptr;
  void* user_ = nullptr;
};

}  // namespace basic
}  // namespace a2ui_lvgl

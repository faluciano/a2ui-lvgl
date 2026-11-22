// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0

#include "basic_common.h"

#include <cstring>

namespace a2ui_lvgl {
namespace basic {

using a2ui::Json;

namespace {

const char kStylesKey = 0;

// Feedback label user data: which kind of message it shows.
char kShowsErrors = 0;
char kShowsHints = 0;

}  // namespace

Styles::Styles(const Theme& t) : theme(t) {
  lv_style_init(&container);
  lv_style_set_pad_all(&container, 0);
  lv_style_set_pad_row(&container, 0);
  lv_style_set_pad_column(&container, 0);
  lv_style_set_bg_opa(&container, LV_OPA_TRANSP);
  lv_style_set_border_width(&container, 0);

  lv_style_init(&leaf);
  lv_style_set_margin_top(&leaf, t.leaf_margin);
  lv_style_set_margin_bottom(&leaf, t.leaf_margin);
  lv_style_set_margin_left(&leaf, t.leaf_margin);
  lv_style_set_margin_right(&leaf, t.leaf_margin);

  lv_style_init(&card);
  lv_style_set_bg_opa(&card, LV_OPA_TRANSP);
  lv_style_set_border_color(&card, t.outline);
  lv_style_set_border_width(&card, 1);
  lv_style_set_border_opa(&card, LV_OPA_COVER);
  lv_style_set_radius(&card, t.card_radius);
  lv_style_set_pad_all(&card, t.card_padding);
  lv_style_set_pad_row(&card, 0);
  lv_style_set_pad_column(&card, 0);

  lv_style_init(&button);
  lv_style_set_radius(&button, t.control_radius);
  lv_style_set_pad_all(&button, t.button_padding);
  lv_style_set_pad_row(&button, 0);
  lv_style_set_pad_column(&button, 0);
  lv_style_set_border_width(&button, 0);
  lv_style_set_bg_opa(&button, LV_OPA_TRANSP);

  lv_style_init(&button_default);
  lv_style_set_bg_color(&button_default, t.control);
  lv_style_set_bg_opa(&button_default, LV_OPA_COVER);
  lv_style_set_border_color(&button_default, t.outline);
  lv_style_set_border_width(&button_default, 1);
  lv_style_set_text_color(&button_default, t.on_background);

  lv_style_init(&button_primary);
  lv_style_set_bg_color(&button_primary, t.primary);
  lv_style_set_bg_opa(&button_primary, LV_OPA_COVER);
  lv_style_set_text_color(&button_primary, t.on_primary);

  lv_style_init(&button_borderless);
  lv_style_set_bg_opa(&button_borderless, LV_OPA_TRANSP);
  lv_style_set_border_width(&button_borderless, 0);
  lv_style_set_text_color(&button_borderless, t.primary);

  lv_style_init(&pressed);
  lv_style_set_bg_color(&pressed, t.outline);
  lv_style_set_bg_opa(&pressed, LV_OPA_COVER);

  lv_style_init(&disabled);
  lv_style_set_opa(&disabled, LV_OPA_50);

  lv_style_init(&caption);
  lv_style_set_text_color(&caption, t.muted);
  if (t.caption) lv_style_set_text_font(&caption, t.caption);

  lv_style_init(&error_text);
  lv_style_set_text_color(&error_text, t.error);
  if (t.caption) lv_style_set_text_font(&error_text, t.caption);
  lv_style_init(&warning_text);
  lv_style_set_text_color(&warning_text, t.warning);
  if (t.caption) lv_style_set_text_font(&warning_text, t.caption);
  lv_style_init(&info_text);
  lv_style_set_text_color(&info_text, t.info);
  if (t.caption) lv_style_set_text_font(&info_text, t.caption);

  lv_style_init(&divider);
  lv_style_set_bg_color(&divider, t.outline);
  lv_style_set_bg_opa(&divider, LV_OPA_COVER);

  lv_style_init(&field);
  lv_style_set_bg_color(&field, t.background);
  lv_style_set_bg_opa(&field, LV_OPA_COVER);
  lv_style_set_border_color(&field, t.outline);
  lv_style_set_border_width(&field, 1);
  lv_style_set_radius(&field, t.control_radius);
  lv_style_set_pad_all(&field, t.field_padding);
  lv_style_set_text_color(&field, t.on_background);

  lv_style_init(&field_focused);
  lv_style_set_border_color(&field_focused, t.primary);
  lv_style_set_border_width(&field_focused, 2);

  lv_style_init(&chip);
  lv_style_set_radius(&chip, LV_RADIUS_CIRCLE);
  lv_style_set_bg_color(&chip, t.background);
  lv_style_set_bg_opa(&chip, LV_OPA_COVER);
  lv_style_set_border_color(&chip, t.outline);
  lv_style_set_border_width(&chip, 1);
  lv_style_set_pad_hor(&chip, t.field_padding * 2);
  lv_style_set_pad_ver(&chip, t.field_padding);
  lv_style_set_text_color(&chip, t.on_background);
  lv_style_set_margin_right(&chip, t.leaf_margin);
  lv_style_set_margin_bottom(&chip, t.leaf_margin);

  lv_style_init(&chip_checked);
  lv_style_set_bg_color(&chip_checked, t.primary);
  lv_style_set_border_color(&chip_checked, t.primary);
  lv_style_set_text_color(&chip_checked, t.on_primary);

  lv_style_init(&tab);
  lv_style_set_bg_opa(&tab, LV_OPA_TRANSP);
  lv_style_set_pad_hor(&tab, t.field_padding * 2);
  lv_style_set_pad_ver(&tab, t.field_padding);
  lv_style_set_border_side(&tab, LV_BORDER_SIDE_BOTTOM);
  lv_style_set_border_width(&tab, 2);
  lv_style_set_border_color(&tab, t.outline);
  lv_style_set_border_opa(&tab, LV_OPA_TRANSP);
  lv_style_set_text_color(&tab, t.muted);

  lv_style_init(&tab_active);
  lv_style_set_border_opa(&tab_active, LV_OPA_COVER);
  lv_style_set_border_color(&tab_active, t.primary);
  lv_style_set_text_color(&tab_active, t.primary);

  lv_style_init(&placeholder_box);
  lv_style_set_bg_color(&placeholder_box, t.control);
  lv_style_set_bg_opa(&placeholder_box, LV_OPA_COVER);
  lv_style_set_radius(&placeholder_box, t.control_radius);
  lv_style_set_clip_corner(&placeholder_box, true);
  lv_style_set_text_color(&placeholder_box, t.muted);

  lv_style_init(&scrim);
  lv_style_set_bg_color(&scrim, t.scrim);
  lv_style_set_bg_opa(&scrim, t.scrim_opa);
  lv_style_set_border_width(&scrim, 0);
  lv_style_set_radius(&scrim, 0);
  lv_style_set_pad_all(&scrim, 0);

  lv_style_init(&dialog);
  lv_style_set_bg_color(&dialog, t.dialog);
  lv_style_set_bg_opa(&dialog, LV_OPA_COVER);
  lv_style_set_radius(&dialog, t.card_radius);
  lv_style_set_pad_all(&dialog, t.card_padding);
  lv_style_set_pad_row(&dialog, 0);
  lv_style_set_border_width(&dialog, 0);
  lv_style_set_text_color(&dialog, t.on_background);

  lv_style_init(&radio_indicator);
  lv_style_set_radius(&radio_indicator, LV_RADIUS_CIRCLE);
}

Styles::~Styles() {
  for (lv_style_t* s : {&container, &leaf, &card, &button, &button_default, &button_primary, &button_borderless,
                        &pressed, &disabled, &caption, &error_text, &warning_text, &info_text, &divider, &field,
                        &field_focused, &chip, &chip_checked, &tab, &tab_active, &placeholder_box, &scrim, &dialog,
                        &radio_indicator}) {
    lv_style_reset(s);
  }
}

Styles& styles(Surface& surface) {
  std::shared_ptr<void>& slot = surface.resource(&kStylesKey);
  // The resource slot is a public std::shared_ptr<void>; one per surface.
  if (!slot) slot = std::make_shared<Styles>(surface.theme());  // ref-check: allow
  return *static_cast<Styles*>(slot.get());
}

Styles& styles(NodeView& view) { return styles(view.surface()); }

lv_obj_t* make_container(lv_obj_t* parent, const Styles& s) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_add_style(o, const_cast<lv_style_t*>(&s.container), 0);
  lv_obj_set_clickable(o, false);
  lv_obj_set_scrollable(o, false);
  lv_obj_set_scroll_on_focus(o, false);
  lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  return o;
}

void set_label_text(lv_obj_t* label, const std::string& text) {
  const char* cur = lv_label_get_text(label);
  if (cur && text == cur) return;
  lv_label_set_text(label, text.c_str());
}

bool set_width(lv_obj_t* obj, int32_t w) {
  if (lv_obj_get_style_width(obj, LV_PART_MAIN) == w) return false;
  lv_obj_set_width(obj, w);
  return true;
}

void set_height(lv_obj_t* obj, int32_t h) {
  if (lv_obj_get_style_height(obj, LV_PART_MAIN) != h) lv_obj_set_height(obj, h);
}

lv_flex_align_t main_place(std::string_view justify) {
  if (justify == "center") return LV_FLEX_ALIGN_CENTER;
  if (justify == "end") return LV_FLEX_ALIGN_END;
  if (justify == "spaceBetween") return LV_FLEX_ALIGN_SPACE_BETWEEN;
  if (justify == "spaceAround") return LV_FLEX_ALIGN_SPACE_AROUND;
  if (justify == "spaceEvenly") return LV_FLEX_ALIGN_SPACE_EVENLY;
  return LV_FLEX_ALIGN_START;
}

lv_flex_align_t cross_place(std::string_view align) {
  if (align == "center") return LV_FLEX_ALIGN_CENTER;
  if (align == "end") return LV_FLEX_ALIGN_END;
  return LV_FLEX_ALIGN_START;
}

namespace {

/** Re-runs a child's update after its width mode changed, so it re-lays out its own children. */
void relayout(NodeView* v) {
  if (ComponentInstance* inst = v->instance()) inst->update(*v);
}

}  // namespace

void fit_to_content(const std::vector<NodeView*>& kids) {
  for (NodeView* kid : kids) {
    lv_obj_t* o = kid->obj();
    if (!o) continue;
    // A percentage cap inside a content-sized parent resolves to zero.
    if (LV_COORD_IS_PCT(lv_obj_get_style_max_width(o, LV_PART_MAIN))) {
      lv_obj_set_style_max_width(o, LV_COORD_MAX, 0);
    }
    if (!LV_COORD_IS_PCT(lv_obj_get_style_width(o, LV_PART_MAIN))) continue;
    if (set_width(o, LV_SIZE_CONTENT)) relayout(kid);
  }
}

void layout_children(lv_obj_t* container, const std::vector<NodeView*>& kids, Axis axis, std::string_view align,
                     std::string_view justify) {
  const bool stretch_cross = align.empty() || align == "stretch";
  const bool stretch_main = justify == "stretch";
  // LVGL ignores percentage-sized children when it sizes a container to its
  // content, so a content-width container lays its children out at content
  // width too (CSS would use their max-content width).
  const bool definite_width = lv_obj_get_style_width(container, LV_PART_MAIN) != LV_SIZE_CONTENT;
  for (NodeView* kid : kids) {
    lv_obj_t* o = kid->obj();
    if (!o) continue;
    const double weight = kid->number_prop("weight", 0);
    // Only children without a fixed size stretch or grow (as in CSS, where
    // stretch applies to auto sizes): an icon keeps its 24 px.
    const int32_t w = lv_obj_get_style_width(o, LV_PART_MAIN);
    const bool fills = LV_COORD_IS_PCT(w);
    const bool flexible = w == LV_SIZE_CONTENT || fills;
    uint8_t grow = 0;
    if (weight > 0) {
      grow = static_cast<uint8_t>(weight > 255 ? 255 : (weight < 1 ? 1 : weight));
    } else if (stretch_main && flexible) {
      grow = 1;
    } else if (axis == Axis::Row && fills && definite_width) {
      // A full-width child of a row (a Column, a Card, a field) takes an
      // equal share of the free space instead of the whole row.
      grow = 1;
    }
    if (lv_obj_get_style_flex_grow(o, LV_PART_MAIN) != grow) lv_obj_set_flex_grow(o, grow);
    bool changed = false;
    // LVGL wraps a label only at a definite width, so in a column text takes
    // the full width and aligns itself instead (it has no box of its own).
    const bool text_like = lv_obj_check_type(o, &lv_label_class) || lv_obj_check_type(o, &lv_spangroup_class);
    if (axis == Axis::Column && text_like && definite_width) {
      changed = set_width(o, lv_pct(100));
      const lv_text_align_t ta = align == "center" ? LV_TEXT_ALIGN_CENTER
                                 : align == "end"  ? LV_TEXT_ALIGN_RIGHT
                                                   : LV_TEXT_ALIGN_AUTO;
      if (lv_obj_get_style_text_align(o, LV_PART_MAIN) != ta) lv_obj_set_style_text_align(o, ta, 0);
    } else if (axis == Axis::Column) {
      if (stretch_cross && flexible) changed = set_width(o, definite_width ? lv_pct(100) : LV_SIZE_CONTENT);
    } else if (!definite_width && fills) {
      changed = set_width(o, LV_SIZE_CONTENT);
    }
    // Content-sized children never grow past the container (text wraps);
    // inside a content-sized container a percentage cap would be zero.
    if (flexible) {
      const int32_t cap = definite_width ? lv_pct(100) : LV_COORD_MAX;
      if (lv_obj_get_style_max_width(o, LV_PART_MAIN) != cap) lv_obj_set_style_max_width(o, cap, 0);
    }
    // The child lays out its own children by whether its width is definite.
    if (changed && (w == LV_SIZE_CONTENT) != (lv_obj_get_style_width(o, LV_PART_MAIN) == LV_SIZE_CONTENT)) {
      relayout(kid);
    }
  }
}

std::string subtree_text(const NodeView& view) {
  std::vector<std::string> parts;
  for (NodeView* c : view.children()) {
    Accessibility a = c->accessibility();
    if (a.hidden) continue;
    std::string t = !a.label.empty() ? a.label : subtree_text(*c);
    if (!t.empty()) parts.push_back(std::move(t));
  }
  return join(parts, " ");
}

std::vector<std::string> split_lines(const std::string& s) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    size_t nl = s.find('\n', start);
    if (nl == std::string::npos) nl = s.size();
    if (nl > start) out.push_back(s.substr(start, nl - start));
    start = nl + 1;
  }
  return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) out += sep;
    out += parts[i];
  }
  return out;
}

CheckFeedback check_feedback(const NodeView& view) {
  CheckFeedback fb;
  const Json errors = view.node().validation_errors();
  for (const auto& e : errors.as_array()) {
    if (e.is_string() && !e.as_string().empty()) fb.errors.push_back(e.as_string());
  }
  const Json results = view.node().validation_results();
  for (const auto& r : results.as_array()) {
    const std::string severity = r["severity"].is_string() ? r["severity"].as_string() : "error";
    if (severity == "error") continue;
    if (r["valid"].is_bool() && r["valid"].as_bool()) continue;
    if (r["message"].is_string() && !r["message"].as_string().empty()) {
      fb.hints.push_back(r["message"].as_string());
      if (severity == "warning" || fb.hint_severity.empty()) fb.hint_severity = severity;
    }
  }
  return fb;
}

void show_feedback(lv_obj_t* label, const CheckFeedback& fb, const Styles& s) {
  lv_obj_remove_style(label, const_cast<lv_style_t*>(&s.error_text), 0);
  lv_obj_remove_style(label, const_cast<lv_style_t*>(&s.warning_text), 0);
  lv_obj_remove_style(label, const_cast<lv_style_t*>(&s.info_text), 0);
  if (!fb.errors.empty()) {
    lv_obj_add_style(label, const_cast<lv_style_t*>(&s.error_text), 0);
    set_label_text(label, join(fb.errors, "\n"));
    lv_obj_set_user_data(label, &kShowsErrors);
    lv_obj_set_hidden(label, false);
  } else if (!fb.hints.empty()) {
    lv_obj_add_style(label, const_cast<lv_style_t*>(fb.hint_severity == "warning" ? &s.warning_text : &s.info_text),
                     0);
    set_label_text(label, join(fb.hints, "\n"));
    lv_obj_set_user_data(label, &kShowsHints);
    lv_obj_set_hidden(label, false);
  } else {
    set_label_text(label, "");
    lv_obj_set_user_data(label, nullptr);
    lv_obj_set_hidden(label, true);
  }
}

void describe_feedback(lv_obj_t* label, Json* out) {
  Json errors = Json::array();
  Json hints = Json::array();
  if (label && !lv_obj_is_hidden(label)) {
    const std::string text = lv_label_get_text(label);
    Json* target = lv_obj_get_user_data(label) == &kShowsErrors ? &errors : &hints;
    for (auto& line : split_lines(text)) target->push_back(Json(line));
  }
  out->set("validationErrors", std::move(errors));
  out->set("hints", std::move(hints));
}

bool is_disabled(lv_obj_t* obj) { return obj && lv_obj_has_state(obj, LV_STATE_DISABLED); }

}  // namespace basic
}  // namespace a2ui_lvgl

namespace a2ui_lvgl {
namespace basic {

lv_obj_t* Popup::open(lv_obj_t* anchor, const Styles& s, void (*on_dismiss)(void* user), void* user) {
  if (overlay_) return panel_;
  on_dismiss_ = on_dismiss;
  user_ = user;
  lv_display_t* disp = lv_obj_get_display(anchor);
  overlay_ = lv_obj_create(lv_display_get_layer_top(disp));
  lv_obj_remove_style_all(overlay_);
  lv_obj_add_style(overlay_, const_cast<lv_style_t*>(&s.scrim), 0);
  lv_obj_set_size(overlay_, lv_pct(100), lv_pct(100));
  lv_obj_set_clickable(overlay_, true);
  lv_obj_set_scrollable(overlay_, false);
  lv_obj_add_event_cb(overlay_, &Popup::on_event, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(overlay_, &Popup::on_event, LV_EVENT_DELETE, this);
  // The panel inherits the surface's text colour and font through the theme,
  // not through the (foreign) top layer.
  lv_obj_set_style_text_color(overlay_, s.theme.on_background, 0);
  if (s.theme.body) lv_obj_set_style_text_font(overlay_, s.theme.body, 0);

  panel_ = lv_obj_create(overlay_);
  lv_obj_remove_style_all(panel_);
  lv_obj_add_style(panel_, const_cast<lv_style_t*>(&s.dialog), 0);
  lv_obj_set_width(panel_, lv_pct(90));
  lv_obj_set_style_max_width(panel_, 480, 0);
  lv_obj_set_height(panel_, LV_SIZE_CONTENT);
  lv_obj_set_style_max_height(panel_, lv_pct(90), 0);
  lv_obj_center(panel_);
  lv_obj_set_flex_flow(panel_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(panel_, LV_DIR_VER);
  return panel_;
}

void Popup::close() {
  if (!overlay_) return;
  lv_obj_t* o = overlay_;
  overlay_ = nullptr;
  panel_ = nullptr;
  lv_obj_remove_event_cb_with_user_data(o, &Popup::on_event, this);
  lv_obj_delete(o);
}

void Popup::on_event(lv_event_t* e) {
  auto* self = static_cast<Popup*>(lv_event_get_user_data(e));
  if (lv_event_get_code(e) == LV_EVENT_DELETE) {
    self->overlay_ = nullptr;
    self->panel_ = nullptr;
    return;
  }
  if (lv_event_get_target(e) != self->overlay_) return;  // A click inside the panel.
  if (self->on_dismiss_) self->on_dismiss_(self->user_);
}

}  // namespace basic
}  // namespace a2ui_lvgl

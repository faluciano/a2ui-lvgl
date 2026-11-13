// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Button, CheckBox, TextField, Slider and ChoicePicker.
//
// Interaction rules (blueprint §3, §5):
//   - actions are invoked synchronously inside the LVGL event handler (the
//     user gesture), never deferred;
//   - user input is written back through the node's writable binding as it
//     happens; programmatic updates from props are guarded so they do not
//     echo back as writes;
//   - after node->invoke()/write() returns, a handler does not touch the
//     instance again: the write may have re-rendered (or removed) the view.
//   - invalid checks disable a Button; error messages show under a field,
//     warning/info results as non-blocking hints.

#include <cmath>
#include <cstring>

#include "a2ui/coerce.h"
#include "basic_components.h"

namespace a2ui_lvgl {
namespace basic {

namespace {

using a2ui::Json;

/** A caption label (hidden while empty). */
lv_obj_t* make_caption(lv_obj_t* parent, const Styles& s) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_add_style(l, const_cast<lv_style_t*>(&s.caption), 0);
  lv_label_set_text_static(l, "");
  lv_obj_set_hidden(l, true);
  return l;
}

void set_caption(lv_obj_t* label, const std::string& text) {
  set_label_text(label, text);
  if (text.empty()) {
    lv_obj_set_hidden(label, true);
  } else {
    lv_obj_set_hidden(label, false);
  }
}

lv_obj_t* make_feedback(lv_obj_t* parent) {
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_width(l, lv_pct(100));
  lv_label_set_text_static(l, "");
  lv_obj_set_hidden(l, true);
  return l;
}

/** The column every labelled control lives in: carries the leaf margin. */
lv_obj_t* make_field_root(lv_obj_t* parent, const Styles& s) {
  lv_obj_t* root = make_container(parent, s);
  lv_obj_add_style(root, const_cast<lv_style_t*>(&s.leaf), 0);
  lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_width(root, lv_pct(100));
  lv_obj_set_style_pad_row(root, 2, 0);
  return root;
}

void set_state(lv_obj_t* obj, lv_state_t state, bool on) {
  if (lv_obj_has_state(obj, state) == on) return;
  if (on) {
    lv_obj_add_state(obj, state);
  } else {
    lv_obj_remove_state(obj, state);
  }
}

// --- Button ---------------------------------------------------------------------

class Button : public ComponentInstance {
 public:
  lv_obj_t* mount(NodeView& view, lv_obj_t* parent) override {
    view_ = &view;
    const Styles& s = styles(view);
    obj_ = lv_button_create(parent);
    lv_obj_remove_style_all(obj_);
    lv_obj_add_style(obj_, const_cast<lv_style_t*>(&s.button), 0);
    lv_obj_add_style(obj_, const_cast<lv_style_t*>(&s.leaf), 0);
    lv_obj_add_style(obj_, const_cast<lv_style_t*>(&s.pressed), LV_STATE_PRESSED);
    lv_obj_add_style(obj_, const_cast<lv_style_t*>(&s.disabled), LV_STATE_DISABLED);
    lv_obj_set_size(obj_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(obj_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(obj_, false);
    add_event<Button, &Button::on_click>(obj_, LV_EVENT_CLICKED, this);
    return obj_;
  }

  void update(NodeView& view) override {
    const Styles& s = styles(view);
    const std::string variant = view.string_prop("variant", "default");
    if (variant != variant_) {
      lv_obj_remove_style(obj_, const_cast<lv_style_t*>(&s.button_default), 0);
      lv_obj_remove_style(obj_, const_cast<lv_style_t*>(&s.button_primary), 0);
      lv_obj_remove_style(obj_, const_cast<lv_style_t*>(&s.button_borderless), 0);
      const lv_style_t* v = variant == "primary"      ? &s.button_primary
                            : variant == "borderless" ? &s.button_borderless
                                                      : &s.button_default;
      lv_obj_add_style(obj_, const_cast<lv_style_t*>(v), 0);
      variant_ = variant;
    }
    set_state(obj_, LV_STATE_DISABLED, !view.node().is_valid());
    std::vector<a2ui::ComponentNode*> nodes;
    if (a2ui::ComponentNode* child = view.child_node("child")) nodes.push_back(child);
    fit_to_content(view.sync_slot(0, obj_, nodes));
  }

  void infer_accessibility(const NodeView& view, Accessibility* out) const override {
    out->role = "button";
    out->label = subtree_text(view);
    out->disabled = is_disabled(obj_);
  }

  bool simulate_gesture(NodeView&) override {
    lv_obj_send_event(obj_, LV_EVENT_CLICKED, nullptr);
    return true;
  }

 private:
  void on_click(lv_event_t*) {
    if (is_disabled(obj_)) return;
    a2ui::NodePtr node = view_->node_ptr();  // `this` may be gone after invoke().
    node->invoke("action");
  }

  NodeView* view_ = nullptr;
  lv_obj_t* obj_ = nullptr;
  std::string variant_;
};

// --- CheckBox ---------------------------------------------------------------------

class CheckBox : public ComponentInstance {
 public:
  lv_obj_t* mount(NodeView& view, lv_obj_t* parent) override {
    view_ = &view;
    const Styles& s = styles(view);
    root_ = make_field_root(parent, s);
    lv_obj_set_width(root_, LV_SIZE_CONTENT);
    box_ = lv_checkbox_create(root_);
    lv_checkbox_set_text(box_, "");
    feedback_ = make_feedback(root_);
    add_event<CheckBox, &CheckBox::on_changed>(box_, LV_EVENT_VALUE_CHANGED, this);
    return root_;
  }

  void update(NodeView& view) override {
    const std::string label = view.string_prop("label");
    const char* cur = lv_checkbox_get_text(box_);
    if (!cur || label != cur) lv_checkbox_set_text(box_, label.c_str());
    set_state(box_, LV_STATE_CHECKED, view.bool_prop("value", false));
    show_feedback(feedback_, check_feedback(view), styles(view));
  }

  void describe(const NodeView&, Json* out) const override {
    out->set("value", Json(lv_obj_has_state(box_, LV_STATE_CHECKED)));
    out->set("label", Json(lv_checkbox_get_text(box_)));
    describe_feedback(feedback_, out);
  }

  void infer_accessibility(const NodeView& view, Accessibility* out) const override {
    out->role = "checkbox";
    out->label = view.string_prop("label");
    out->checked = lv_obj_has_state(box_, LV_STATE_CHECKED);
  }

  bool simulate_input(NodeView&, const Json& value) override {
    const bool want = a2ui::coerce_bool(value);
    if (lv_obj_has_state(box_, LV_STATE_CHECKED) == want) return true;
    set_state(box_, LV_STATE_CHECKED, want);
    lv_obj_send_event(box_, LV_EVENT_VALUE_CHANGED, nullptr);
    return true;
  }

 private:
  void on_changed(lv_event_t*) {
    const bool checked = lv_obj_has_state(box_, LV_STATE_CHECKED);
    a2ui::NodePtr node = view_->node_ptr();
    node->write("value", Json(checked));
  }

  NodeView* view_ = nullptr;
  lv_obj_t* root_ = nullptr;
  lv_obj_t* box_ = nullptr;
  lv_obj_t* feedback_ = nullptr;
};

// --- TextField ----------------------------------------------------------------------

class TextField : public ComponentInstance {
 public:
  lv_obj_t* mount(NodeView& view, lv_obj_t* parent) override {
    view_ = &view;
    const Styles& s = styles(view);
    root_ = make_field_root(parent, s);
    label_ = make_caption(root_, s);
    ta_ = lv_textarea_create(root_);
    lv_obj_add_style(ta_, const_cast<lv_style_t*>(&s.field), 0);
    lv_obj_add_style(ta_, const_cast<lv_style_t*>(&s.field_focused), LV_STATE_FOCUSED);
    lv_obj_set_width(ta_, lv_pct(100));
    lv_textarea_set_one_line(ta_, true);
    feedback_ = make_feedback(root_);
    add_event<TextField, &TextField::on_changed>(ta_, LV_EVENT_VALUE_CHANGED, this);
    add_event<TextField, &TextField::on_focus>(ta_, LV_EVENT_FOCUSED, this);
    add_event<TextField, &TextField::on_focus>(ta_, LV_EVENT_DEFOCUSED, this);
    return root_;
  }

  void unmount(NodeView& view) override {
    // Let the host hide an on-screen keyboard bound to this text area.
    if (ta_ && view.obj() && lv_obj_has_state(ta_, LV_STATE_FOCUSED)) {
      if (const auto& hook = view.surface().services().on_text_focus) hook(ta_, false);
    }
  }

  void update(NodeView& view) override {
    set_caption(label_, view.string_prop("label"));
    const std::string variant = view.string_prop("variant", "shortText");
    if (variant != variant_) {
      variant_ = variant;
      updating_ = true;
      const bool multi = variant == "longText";
      lv_textarea_set_one_line(ta_, !multi);
      if (multi) {
        lv_obj_set_height(ta_, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(ta_, 3 * lv_font_get_line_height(lv_obj_get_style_text_font(ta_, LV_PART_MAIN)) +
                                             2 * view.theme().field_padding, 0);
      }
      lv_textarea_set_password_mode(ta_, variant == "obscured");
      lv_textarea_set_accepted_chars(ta_, variant == "number" ? "0123456789.,-+eE" : nullptr);
      updating_ = false;
    }
    const std::string placeholder = view.string_prop("placeholder");
    const char* cur_ph = lv_textarea_get_placeholder_text(ta_);
    if (!cur_ph || placeholder != cur_ph) lv_textarea_set_placeholder_text(ta_, placeholder.c_str());
    const std::string value = view.string_prop("value");
    const char* cur = lv_textarea_get_text(ta_);
    if (!cur || value != cur) {
      updating_ = true;
      lv_textarea_set_text(ta_, value.c_str());
      updating_ = false;
    }
    show_feedback(feedback_, check_feedback(view), styles(view));
  }

  void describe(const NodeView&, Json* out) const override {
    out->set("value", Json(lv_textarea_get_text(ta_)));
    out->set("label", Json(lv_label_get_text(label_)));
    describe_feedback(feedback_, out);
  }

  void infer_accessibility(const NodeView& view, Accessibility* out) const override {
    out->role = "textbox";
    out->label = view.string_prop("label");
  }

  bool simulate_input(NodeView&, const Json& value) override {
    const std::string text = value.is_string() ? value.as_string() : a2ui::coerce_string(value);
    lv_textarea_set_text(ta_, text.c_str());  // Sends LV_EVENT_VALUE_CHANGED like typing does.
    return true;
  }

 private:
  void on_changed(lv_event_t*) {
    if (updating_) return;
    a2ui::NodePtr node = view_->node_ptr();
    node->write("value", Json(lv_textarea_get_text(ta_)));
  }

  void on_focus(lv_event_t* e) {
    if (const auto& hook = view_->surface().services().on_text_focus) {
      hook(ta_, lv_event_get_code(e) == LV_EVENT_FOCUSED);
    }
  }

  NodeView* view_ = nullptr;
  lv_obj_t* root_ = nullptr;
  lv_obj_t* label_ = nullptr;
  lv_obj_t* ta_ = nullptr;
  lv_obj_t* feedback_ = nullptr;
  std::string variant_;
  bool updating_ = false;
};

// --- Slider -------------------------------------------------------------------------

class Slider : public ComponentInstance {
 public:
  lv_obj_t* mount(NodeView& view, lv_obj_t* parent) override {
    view_ = &view;
    const Styles& s = styles(view);
    root_ = make_field_root(parent, s);
    header_ = make_container(root_, s);
    lv_obj_set_width(header_, lv_pct(100));
    lv_obj_set_flex_flow(header_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    label_ = lv_label_create(header_);
    lv_label_set_text_static(label_, "");
    value_label_ = lv_label_create(header_);
    lv_obj_add_style(value_label_, const_cast<lv_style_t*>(&s.caption), 0);
    lv_label_set_text_static(value_label_, "");
    // Room for the knob, which draws outside the track.
    track_box_ = make_container(root_, s);
    lv_obj_set_width(track_box_, lv_pct(100));
    lv_obj_set_style_pad_hor(track_box_, 10, 0);
    lv_obj_set_style_pad_ver(track_box_, 8, 0);
    slider_ = lv_slider_create(track_box_);
    lv_obj_set_width(slider_, lv_pct(100));
    feedback_ = make_feedback(root_);
    add_event<Slider, &Slider::on_changed>(slider_, LV_EVENT_VALUE_CHANGED, this);
    return root_;
  }

  void update(NodeView& view) override {
    const std::string label = view.string_prop("label");
    set_label_text(label_, label);
    lv_obj_set_hidden(header_, label.empty());
    min_ = view.number_prop("min", 0);
    max_ = view.number_prop("max", 100);
    if (!(max_ > min_)) max_ = min_ + 1;
    steps_ = static_cast<int32_t>(view.number_prop("steps", 0));
    if (steps_ > 0) {
      mode_ = Mode::Steps;
      lv_slider_set_range(slider_, 0, steps_);
    } else if (std::floor(min_) == min_ && std::floor(max_) == max_ && max_ - min_ >= 100 &&
               max_ - min_ <= 100000 && std::fabs(min_) < 1e9 && std::fabs(max_) < 1e9) {
      // Whole-number range with enough positions: one step per unit.
      mode_ = Mode::Direct;
      lv_slider_set_range(slider_, static_cast<int32_t>(min_), static_cast<int32_t>(max_));
    } else {
      // Fractional or short ranges (0..1): 1000 positions.
      mode_ = Mode::Scaled;
      lv_slider_set_range(slider_, 0, kScale);
    }
    const double value = view.number_prop("value", min_);
    const int32_t pos = to_pos(value);
    if (lv_slider_get_value(slider_) != pos) lv_slider_set_value(slider_, pos, LV_ANIM_OFF);
    show_value(value);
    show_feedback(feedback_, check_feedback(view), styles(view));
  }

  void describe(const NodeView&, Json* out) const override {
    out->set("value", Json(from_pos(lv_slider_get_value(slider_))));
    out->set("label", Json(lv_label_get_text(label_)));
  }

  void infer_accessibility(const NodeView& view, Accessibility* out) const override {
    out->role = "slider";
    out->label = view.string_prop("label");
  }

  bool simulate_input(NodeView&, const Json& value) override {
    lv_slider_set_value(slider_, to_pos(a2ui::coerce_number(value)), LV_ANIM_OFF);
    lv_obj_send_event(slider_, LV_EVENT_VALUE_CHANGED, nullptr);
    return true;
  }

 private:
  enum class Mode { Direct, Steps, Scaled };
  static constexpr int32_t kScale = 1000;

  int32_t to_pos(double v) const {
    if (v < min_) v = min_;
    if (v > max_) v = max_;
    switch (mode_) {
      case Mode::Direct:
        return static_cast<int32_t>(std::lround(v));
      case Mode::Steps:
        return static_cast<int32_t>(std::lround((v - min_) / (max_ - min_) * steps_));
      case Mode::Scaled:
        break;
    }
    return static_cast<int32_t>(std::lround((v - min_) / (max_ - min_) * kScale));
  }

  double from_pos(int32_t pos) const {
    switch (mode_) {
      case Mode::Direct:
        return pos;
      case Mode::Steps:
        return min_ + (max_ - min_) * pos / steps_;
      case Mode::Scaled:
        break;
    }
    return min_ + (max_ - min_) * pos / kScale;
  }

  void show_value(double v) {
    const double rounded = mode_ == Mode::Direct ? std::round(v) : std::round(v * 100) / 100;
    set_label_text(value_label_, a2ui::number_to_string(rounded));
  }

  void on_changed(lv_event_t*) {
    const double v = from_pos(lv_slider_get_value(slider_));
    show_value(v);
    a2ui::NodePtr node = view_->node_ptr();
    node->write("value", Json(v));
  }

  NodeView* view_ = nullptr;
  lv_obj_t* root_ = nullptr;
  lv_obj_t* header_ = nullptr;
  lv_obj_t* label_ = nullptr;
  lv_obj_t* value_label_ = nullptr;
  lv_obj_t* track_box_ = nullptr;
  lv_obj_t* slider_ = nullptr;
  lv_obj_t* feedback_ = nullptr;
  double min_ = 0, max_ = 100;
  int32_t steps_ = 0;
  Mode mode_ = Mode::Direct;
};

// --- ChoicePicker -------------------------------------------------------------------

std::string ascii_lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

class ChoicePicker : public ComponentInstance {
 public:
  lv_obj_t* mount(NodeView& view, lv_obj_t* parent) override {
    view_ = &view;
    const Styles& s = styles(view);
    root_ = make_field_root(parent, s);
    label_ = make_caption(root_, s);
    options_box_ = make_container(root_, s);
    lv_obj_set_width(options_box_, lv_pct(100));
    feedback_ = make_feedback(root_);
    return root_;
  }

  void update(NodeView& view) override {
    const Styles& s = styles(view);
    set_caption(label_, view.string_prop("label"));
    exclusive_ = view.string_prop("variant", "mutuallyExclusive") != "multipleSelection";
    const bool chips = view.string_prop("displayStyle", "checkbox") == "chips";
    const bool filterable = view.bool_prop("filterable", false);

    // Options: [{label, value}], labels possibly bound.
    std::vector<std::pair<std::string, std::string>> options;
    if (const a2ui::PropValue* p = view.prop("options")) {
      const Json opts = p->to_json();
      for (const auto& o : opts.as_array()) {
        std::string value = o["value"].is_string() ? o["value"].as_string() : a2ui::coerce_string(o["value"]);
        std::string label = o["label"].is_string() ? o["label"].as_string() : value;
        options.emplace_back(std::move(label), std::move(value));
      }
    }

    if (filterable && !filter_) {
      filter_ = lv_textarea_create(root_);
      lv_obj_move_to_index(filter_, lv_obj_get_index(options_box_));
      lv_obj_add_style(filter_, const_cast<lv_style_t*>(&s.field), 0);
      lv_obj_add_style(filter_, const_cast<lv_style_t*>(&s.field_focused), LV_STATE_FOCUSED);
      lv_obj_set_width(filter_, lv_pct(100));
      lv_textarea_set_one_line(filter_, true);
      lv_textarea_set_placeholder_text(filter_, "Filter");
      add_event<ChoicePicker, &ChoicePicker::on_filter>(filter_, LV_EVENT_VALUE_CHANGED, this);
    } else if (!filterable && filter_) {
      lv_obj_delete(filter_);
      filter_ = nullptr;
    }

    if (options != options_ || chips != chips_ || exclusive_ != built_exclusive_) {
      rebuild(s, options, chips);
    }
    // Selection.
    const Json& selected = view.value("value");
    for (size_t i = 0; i < option_objs_.size(); ++i) {
      bool on = false;
      for (const auto& v : selected.as_array()) {
        if (v.is_string() && v.as_string() == options_[i].second) on = true;
      }
      set_state(option_objs_[i], LV_STATE_CHECKED, on);
    }
    apply_filter();
    show_feedback(feedback_, check_feedback(view), s);
  }

  void describe(const NodeView&, Json* out) const override {
    out->set("value", selection());
    out->set("label", Json(lv_label_get_text(label_)));
    describe_feedback(feedback_, out);
  }

  void infer_accessibility(const NodeView& view, Accessibility* out) const override {
    out->role = exclusive_ ? "radiogroup" : "group";
    out->label = view.string_prop("label");
  }

  bool simulate_input(NodeView&, const Json& value) override {
    // Click the options whose state differs, as a user would, re-reading
    // the states after each click (an exclusive pick unchecks the others).
    for (int guard = 0; guard < 64; ++guard) {
      bool clicked = false;
      for (size_t i = 0; i < option_objs_.size(); ++i) {
        bool want = false;
        for (const auto& v : value.as_array()) {
          if (v.is_string() && v.as_string() == options_[i].second) want = true;
        }
        if (lv_obj_has_state(option_objs_[i], LV_STATE_CHECKED) == want) continue;
        if (exclusive_ && !want) continue;  // Picking another option clears this one.
        set_state(option_objs_[i], LV_STATE_CHECKED, want);
        lv_obj_send_event(option_objs_[i], LV_EVENT_VALUE_CHANGED, nullptr);
        clicked = true;
        break;
      }
      if (!clicked) break;
    }
    return true;
  }

 private:
  void rebuild(const Styles& s, const std::vector<std::pair<std::string, std::string>>& options, bool chips) {
    lv_obj_clean(options_box_);
    option_objs_.clear();
    options_ = options;
    chips_ = chips;
    built_exclusive_ = exclusive_;
    lv_obj_set_flex_flow(options_box_, chips ? LV_FLEX_FLOW_ROW_WRAP : LV_FLEX_FLOW_COLUMN);
    for (const auto& opt : options_) {
      lv_obj_t* o = nullptr;
      if (chips) {
        o = lv_button_create(options_box_);
        lv_obj_remove_style_all(o);
        lv_obj_add_style(o, const_cast<lv_style_t*>(&s.chip), 0);
        lv_obj_add_style(o, const_cast<lv_style_t*>(&s.chip_checked), LV_STATE_CHECKED);
        lv_obj_set_checkable(o, true);
        lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_t* l = lv_label_create(o);
        lv_label_set_text(l, opt.first.c_str());
      } else {
        o = lv_checkbox_create(options_box_);
        lv_checkbox_set_text(o, opt.first.c_str());
        if (exclusive_) lv_obj_add_style(o, const_cast<lv_style_t*>(&s.radio_indicator), LV_PART_INDICATOR);
        lv_obj_set_style_pad_ver(o, 2, 0);
      }
      add_event<ChoicePicker, &ChoicePicker::on_option>(o, LV_EVENT_VALUE_CHANGED, this);
      option_objs_.push_back(o);
    }
  }

  Json selection() const {
    Json out = Json::array();
    for (size_t i = 0; i < option_objs_.size(); ++i) {
      if (lv_obj_has_state(option_objs_[i], LV_STATE_CHECKED)) out.push_back(Json(options_[i].second));
    }
    return out;
  }

  void on_option(lv_event_t* e) {
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    size_t index = option_objs_.size();
    for (size_t i = 0; i < option_objs_.size(); ++i) {
      if (option_objs_[i] == target) index = i;
    }
    if (index == option_objs_.size()) return;
    Json next = Json::array();
    if (exclusive_) {
      if (!lv_obj_has_state(target, LV_STATE_CHECKED)) {
        // A radio stays selected when tapped again.
        lv_obj_add_state(target, LV_STATE_CHECKED);
        return;
      }
      next.push_back(Json(options_[index].second));
    } else {
      next = selection();
    }
    a2ui::NodePtr node = view_->node_ptr();
    node->write("value", std::move(next));
  }

  void on_filter(lv_event_t*) { apply_filter(); }

  void apply_filter() {
    const std::string needle = filter_ ? ascii_lower(lv_textarea_get_text(filter_)) : std::string();
    for (size_t i = 0; i < option_objs_.size(); ++i) {
      const bool show = needle.empty() || ascii_lower(options_[i].first).find(needle) != std::string::npos;
      if (show) {
        lv_obj_set_hidden(option_objs_[i], false);
      } else {
        lv_obj_set_hidden(option_objs_[i], true);
      }
    }
  }

  NodeView* view_ = nullptr;
  lv_obj_t* root_ = nullptr;
  lv_obj_t* label_ = nullptr;
  lv_obj_t* filter_ = nullptr;
  lv_obj_t* options_box_ = nullptr;
  lv_obj_t* feedback_ = nullptr;
  std::vector<std::pair<std::string, std::string>> options_;
  std::vector<lv_obj_t*> option_objs_;
  bool chips_ = false;
  bool exclusive_ = true;
  bool built_exclusive_ = true;
};

}  // namespace

std::unique_ptr<ComponentInstance> make_button() { return std::make_unique<Button>(); }
std::unique_ptr<ComponentInstance> make_check_box() { return std::make_unique<CheckBox>(); }
std::unique_ptr<ComponentInstance> make_text_field() { return std::make_unique<TextField>(); }
std::unique_ptr<ComponentInstance> make_slider() { return std::make_unique<Slider>(); }
std::unique_ptr<ComponentInstance> make_choice_picker() { return std::make_unique<ChoicePicker>(); }

}  // namespace basic
}  // namespace a2ui_lvgl

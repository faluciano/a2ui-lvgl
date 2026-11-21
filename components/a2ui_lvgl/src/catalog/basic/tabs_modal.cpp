// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Tabs and Modal.
//
// Tabs keeps a local selectedIndex (default 0) and mounts only the selected
// tab's child. Modal shows its trigger in place; a tap on the trigger (the
// trigger's own action runs first, then the event bubbles to the Modal)
// opens a dialog on the display's top layer with a close button and a
// dimmed backdrop that also closes it. The content child is mounted only
// while the dialog is open.

#include "a2ui/coerce.h"
#include "basic_components.h"

namespace a2ui_lvgl {
namespace basic {

namespace {

using a2ui::Json;

class Tabs : public ComponentInstance {
 public:
  lv_obj_t* mount(NodeView& view, lv_obj_t* parent) override {
    view_ = &view;
    const Styles& s = styles(view);
    root_ = make_container(parent, s);
    lv_obj_add_style(root_, const_cast<lv_style_t*>(&s.leaf), 0);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(root_, lv_pct(100));
    header_ = make_container(root_, s);
    lv_obj_set_width(header_, lv_pct(100));
    lv_obj_set_flex_flow(header_, LV_FLEX_FLOW_ROW);
    lv_obj_set_scrollable(header_, true);
    lv_obj_set_scroll_dir(header_, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(header_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_side(header_, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(header_, 1, 0);
    lv_obj_set_style_border_color(header_, view.theme().outline, 0);
    content_ = make_container(root_, s);
    lv_obj_set_width(content_, lv_pct(100));
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    return root_;
  }

  void update(NodeView& view) override {
    const Styles& s = styles(view);
    std::vector<std::string> titles;
    std::vector<a2ui::ComponentNode*> children;
    if (const a2ui::PropValue* tabs = view.prop("tabs")) {
      for (const auto& item : tabs->items()) {
        const a2ui::PropValue* title = item.find("title");
        const a2ui::PropValue* child = item.find("child");
        titles.push_back(title ? a2ui::coerce_string(title->value()) : std::string());
        children.push_back(child ? child->node() : nullptr);
      }
    }
    if (titles.size() != buttons_.size()) {
      lv_obj_clean(header_);
      buttons_.clear();
      for (size_t i = 0; i < titles.size(); ++i) {
        lv_obj_t* b = lv_button_create(header_);
        lv_obj_remove_style_all(b);
        lv_obj_add_style(b, const_cast<lv_style_t*>(&s.tab), 0);
        lv_obj_add_style(b, const_cast<lv_style_t*>(&s.tab_active), LV_STATE_CHECKED);
        lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_label_create(b);
        add_event<Tabs, &Tabs::on_tab>(b, LV_EVENT_CLICKED, this);
        buttons_.push_back(b);
      }
    }
    for (size_t i = 0; i < titles.size(); ++i) set_label_text(lv_obj_get_child(buttons_[i], 0), titles[i]);
    if (selected_ >= titles.size()) selected_ = 0;
    for (size_t i = 0; i < buttons_.size(); ++i) {
      if (i == selected_) {
        lv_obj_add_state(buttons_[i], LV_STATE_CHECKED);
      } else {
        lv_obj_remove_state(buttons_[i], LV_STATE_CHECKED);
      }
    }
    std::vector<a2ui::ComponentNode*> shown;
    if (selected_ < children.size() && children[selected_]) shown.push_back(children[selected_]);
    layout_children(content_, view.sync_slot(0, content_, shown), Axis::Column, "stretch", "start");
  }

  void describe(const NodeView&, Json* out) const override {
    out->set("selectedIndex", Json(static_cast<double>(selected_)));
  }

  void infer_accessibility(const NodeView&, Accessibility* out) const override { out->role = "tablist"; }

  bool simulate_input(NodeView&, const Json& value) override {
    const double index = a2ui::coerce_number(value);
    if (index < 0 || index >= static_cast<double>(buttons_.size())) return false;
    lv_obj_send_event(buttons_[static_cast<size_t>(index)], LV_EVENT_CLICKED, nullptr);
    return true;
  }

 private:
  void on_tab(lv_event_t* e) {
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    for (size_t i = 0; i < buttons_.size(); ++i) {
      if (buttons_[i] == target && i != selected_) {
        selected_ = i;
        update(*view_);
        return;
      }
    }
  }

  NodeView* view_ = nullptr;
  lv_obj_t* root_ = nullptr;
  lv_obj_t* header_ = nullptr;
  lv_obj_t* content_ = nullptr;
  std::vector<lv_obj_t*> buttons_;
  size_t selected_ = 0;
};

class Modal : public ComponentInstance {
 public:
  lv_obj_t* mount(NodeView& view, lv_obj_t* parent) override {
    view_ = &view;
    root_ = make_container(parent, styles(view));
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    add_event<Modal, &Modal::on_trigger>(root_, LV_EVENT_CLICKED, this);
    return root_;
  }

  void update(NodeView& view) override {
    std::vector<a2ui::ComponentNode*> trigger;
    if (a2ui::ComponentNode* t = view.child_node("trigger")) trigger.push_back(t);
    for (NodeView* v : view.sync_slot(0, root_, trigger)) {
      // Taps on the trigger reach the Modal after the trigger's own handler.
      if (lv_obj_t* o = v->obj()) lv_obj_set_event_bubble(o, true);
    }
    if (!popup_.is_open()) {
      if (!view.slot_views(1).empty()) view.clear_slot(1);  // The dialog was deleted from outside.
      return;
    }
    std::vector<a2ui::ComponentNode*> content;
    if (a2ui::ComponentNode* c = view.child_node("content")) content.push_back(c);
    layout_children(content_, view.sync_slot(1, content_, content), Axis::Column, "stretch", "start");
  }

  void unmount(NodeView&) override { popup_.close(); }

  void describe(const NodeView&, Json* out) const override { out->set("open", Json(popup_.is_open())); }

  void infer_accessibility(const NodeView&, Accessibility* out) const override { out->role = "group"; }

  bool simulate_gesture(NodeView&) override {
    lv_obj_send_event(root_, LV_EVENT_CLICKED, nullptr);
    return true;
  }

  void open() {
    if (popup_.is_open()) return;
    const Styles& s = styles(*view_);
    lv_obj_t* panel = popup_.open(root_, s, &Modal::dismiss, this);
    lv_obj_t* bar = make_container(panel, s);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* close = lv_button_create(bar);
    lv_obj_remove_style_all(close);
    lv_obj_add_style(close, const_cast<lv_style_t*>(&s.button), 0);
    lv_obj_add_style(close, const_cast<lv_style_t*>(&s.button_borderless), 0);
    lv_obj_add_style(close, const_cast<lv_style_t*>(&s.pressed), LV_STATE_PRESSED);
    lv_obj_set_size(close, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_t* x = lv_label_create(close);
    lv_label_set_text_static(x, LV_SYMBOL_CLOSE);
    lv_obj_set_style_pad_all(close, s.theme.leaf_margin, 0);
    add_event<Modal, &Modal::on_close>(close, LV_EVENT_CLICKED, this);
    content_ = make_container(panel, s);
    lv_obj_set_width(content_, lv_pct(100));
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    update(*view_);
  }

  void close() {
    view_->clear_slot(1);
    popup_.close();
    content_ = nullptr;
  }

 private:
  static void dismiss(void* self) { static_cast<Modal*>(self)->close(); }
  void on_trigger(lv_event_t*) { open(); }
  void on_close(lv_event_t*) { close(); }

  NodeView* view_ = nullptr;
  lv_obj_t* root_ = nullptr;
  lv_obj_t* content_ = nullptr;
  Popup popup_;
};

}  // namespace

std::unique_ptr<ComponentInstance> make_tabs() { return std::make_unique<Tabs>(); }
std::unique_ptr<ComponentInstance> make_modal() { return std::make_unique<Modal>(); }

}  // namespace basic
}  // namespace a2ui_lvgl

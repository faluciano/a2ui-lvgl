// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Row, Column, List, Card and Divider.
//
// Row, Column and List are invisible structural containers: no padding, no
// gap, no margin (leaf-margin strategy). Card is an outlined container with a
// transparent background (nested cards stay distinct) and carries the leaf
// margin itself.

#include "basic_components.h"

namespace a2ui_lvgl {
namespace basic {

namespace {

class FlexContainer : public ComponentInstance {
 public:
  enum class Kind { Row, Column, List };
  explicit FlexContainer(Kind kind) : kind_(kind) {}

  lv_obj_t* mount(NodeView& view, lv_obj_t* parent) override {
    const Styles& s = styles(view);
    obj_ = make_container(parent, s);
    lv_obj_set_width(obj_, lv_pct(100));
    if (kind_ == Kind::List) {
      lv_obj_set_scrollable(obj_, true);
    }
    return obj_;
  }

  void update(NodeView& view) override {
    const std::string justify = view.string_prop("justify", "start");
    const std::string align = view.string_prop("align", "stretch");
    Axis axis = kind_ == Kind::Row ? Axis::Row : Axis::Column;
    if (kind_ == Kind::List) {
      const bool horizontal = view.string_prop("direction", "vertical") == "horizontal";
      axis = horizontal ? Axis::Row : Axis::Column;
      lv_obj_set_scroll_dir(obj_, horizontal ? LV_DIR_HOR : LV_DIR_VER);
      lv_obj_set_scrollbar_mode(obj_, horizontal ? LV_SCROLLBAR_MODE_OFF : LV_SCROLLBAR_MODE_AUTO);
    }
    const lv_flex_flow_t flow = axis == Axis::Row ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN;
    if (lv_obj_get_style_flex_flow(obj_, LV_PART_MAIN) != flow) lv_obj_set_flex_flow(obj_, flow);
    const lv_flex_align_t cross = cross_place(align);
    lv_obj_set_flex_align(obj_, kind_ == Kind::List ? LV_FLEX_ALIGN_START : main_place(justify), cross, cross);
    const auto& kids = view.sync_slot(0, obj_, view.child_nodes("children"));
    layout_children(obj_, kids, axis, align, kind_ == Kind::List ? std::string_view() : std::string_view(justify));
    if (kind_ == Kind::List && axis == Axis::Row) {
      // Items of a horizontal list size to their content, up to 80% of the list.
      fit_to_content(kids);
      for (NodeView* kid : kids) {
        if (lv_obj_t* o = kid->obj()) lv_obj_set_style_max_width(o, lv_pct(80), 0);
      }
    }
  }

  void infer_accessibility(const NodeView&, Accessibility* out) const override {
    out->role = kind_ == Kind::List ? "list" : "group";
  }

 private:
  Kind kind_;
  lv_obj_t* obj_ = nullptr;
};

class Card : public ComponentInstance {
 public:
  lv_obj_t* mount(NodeView& view, lv_obj_t* parent) override {
    const Styles& s = styles(view);
    obj_ = lv_obj_create(parent);
    lv_obj_remove_style_all(obj_);
    lv_obj_add_style(obj_, const_cast<lv_style_t*>(&s.card), 0);
    lv_obj_add_style(obj_, const_cast<lv_style_t*>(&s.leaf), 0);
    lv_obj_set_clickable(obj_, false);
    lv_obj_set_scrollable(obj_, false);
    lv_obj_set_size(obj_, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(obj_, LV_FLEX_FLOW_COLUMN);
    return obj_;
  }

  void update(NodeView& view) override {
    std::vector<a2ui::ComponentNode*> nodes;
    if (a2ui::ComponentNode* child = view.child_node("child")) nodes.push_back(child);
    layout_children(obj_, view.sync_slot(0, obj_, nodes), Axis::Column, "stretch", "start");
  }

  void infer_accessibility(const NodeView&, Accessibility* out) const override { out->role = "group"; }

 private:
  lv_obj_t* obj_ = nullptr;
};

class Divider : public ComponentInstance {
 public:
  lv_obj_t* mount(NodeView& view, lv_obj_t* parent) override {
    const Styles& s = styles(view);
    obj_ = lv_obj_create(parent);
    lv_obj_remove_style_all(obj_);
    lv_obj_add_style(obj_, const_cast<lv_style_t*>(&s.divider), 0);
    lv_obj_add_style(obj_, const_cast<lv_style_t*>(&s.leaf), 0);
    lv_obj_set_clickable(obj_, false);
    lv_obj_set_scrollable(obj_, false);
    return obj_;
  }

  void update(NodeView& view) override {
    vertical_ = view.string_prop("axis", "horizontal") == "vertical";
    if (vertical_) {
      lv_obj_set_size(obj_, 1, lv_pct(100));
      lv_obj_set_style_min_height(obj_, view.theme().divider_min_length, 0);
    } else {
      lv_obj_set_size(obj_, lv_pct(100), 1);
      lv_obj_set_style_min_height(obj_, 0, 0);
    }
  }

  void describe(const NodeView&, a2ui::Json* out) const override {
    // Read back from the object's shape.
    const bool vertical = lv_obj_get_style_width(obj_, LV_PART_MAIN) == 1;
    out->set("axis", a2ui::Json(vertical ? "vertical" : "horizontal"));
  }

  void infer_accessibility(const NodeView&, Accessibility* out) const override { out->role = "separator"; }

 private:
  lv_obj_t* obj_ = nullptr;
  bool vertical_ = false;
};

}  // namespace

std::unique_ptr<ComponentInstance> make_row() { return std::make_unique<FlexContainer>(FlexContainer::Kind::Row); }
std::unique_ptr<ComponentInstance> make_column() {
  return std::make_unique<FlexContainer>(FlexContainer::Kind::Column);
}
std::unique_ptr<ComponentInstance> make_list() { return std::make_unique<FlexContainer>(FlexContainer::Kind::List); }
std::unique_ptr<ComponentInstance> make_card() { return std::make_unique<Card>(); }
std::unique_ptr<ComponentInstance> make_divider() { return std::make_unique<Divider>(); }

}  // namespace basic
}  // namespace a2ui_lvgl

// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Row, Column, List, Card and Divider.
//
// Row, Column and List are invisible structural containers: no padding, no
// gap, no margin (leaf-margin strategy). Card is an outlined container with a
// transparent background (nested cards stay distinct) and carries the leaf
// margin itself.
//
// LVGL flex items never shrink, so a Row would lay long text out at its
// natural width and overflow. After each flex pass a Row gives its text
// labels a max width that takes the overflow off them in proportion to their
// natural width, never below their longest word (CSS flex-shrink: 1 with a
// min-content floor); the labels then wrap.

#include <cstring>

#include "basic_components.h"

namespace a2ui_lvgl {
namespace basic {

namespace {

/** A text label a Row may narrow: content-sized, not growing, wrapping. */
bool shrinkable(lv_obj_t* o) {
  return lv_obj_check_type(o, &lv_label_class) && lv_obj_get_style_width(o, LV_PART_MAIN) == LV_SIZE_CONTENT &&
         lv_obj_get_style_flex_grow(o, LV_PART_MAIN) == 0 &&
         lv_label_get_long_mode(o) == LV_LABEL_LONG_MODE_WRAP;
}

int32_t text_width(const char* text, lv_obj_t* o) {
  lv_point_t size;
  lv_text_get_size(&size, text, lv_obj_get_style_text_font(o, LV_PART_MAIN),
                   lv_obj_get_style_text_letter_space(o, LV_PART_MAIN),
                   lv_obj_get_style_text_line_space(o, LV_PART_MAIN), LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  return size.x;
}

int32_t pad_x(lv_obj_t* o) {
  return lv_obj_get_style_pad_left(o, LV_PART_MAIN) + lv_obj_get_style_pad_right(o, LV_PART_MAIN);
}

/** Width of the label's longest word (its min-content width), padding included. */
int32_t min_content_width(lv_obj_t* o) {
  // Words longer than the buffer are measured by their start: LVGL breaks
  // such a word anyway when it does not fit.
  char word[48];
  size_t len = 0;
  int32_t longest = 0;
  for (const char* p = lv_label_get_text(o);; ++p) {
    if (*p == ' ' || *p == '\n' || *p == '\0') {
      if (len > 0) {
        word[len] = '\0';
        const int32_t w = text_width(word, o);
        if (w > longest) longest = w;
        len = 0;
      }
      if (*p == '\0') break;
    } else if (len + 1 < sizeof word) {
      word[len++] = *p;
    }
  }
  return longest + pad_x(o);
}

/**
 * LV_EVENT_LAYOUT_CHANGED of a Row: when the children overflow, narrows
 * its text labels. The result depends only on the texts and the other
 * children's sizes, not on the labels' current widths, so the next layout
 * pass reaches the same widths and the layout settles.
 */
void shrink_row_text(lv_event_t* e) {
  lv_obj_t* row = lv_event_get_current_target_obj(e);
  struct Item {
    lv_obj_t* obj;
    int32_t natural;
    int32_t target;
    bool frozen;
  };
  // Rows rarely hold more texts than this; further ones keep their width.
  constexpr int kMaxItems = 8;
  Item items[kMaxItems];
  int count = 0;
  int32_t used = 0;
  int32_t visible = 0;
  const uint32_t n = lv_obj_get_child_count(row);
  for (uint32_t i = 0; i < n; ++i) {
    lv_obj_t* c = lv_obj_get_child(row, static_cast<int32_t>(i));
    if (lv_obj_is_hidden(c) || lv_obj_is_ignore_layout(c) || lv_obj_is_floating(c)) continue;
    ++visible;
    used += lv_obj_get_style_margin_left(c, LV_PART_MAIN) + lv_obj_get_style_margin_right(c, LV_PART_MAIN);
    if (count < kMaxItems && shrinkable(c)) {
      Item& it = items[count++];
      it.obj = c;
      it.natural = text_width(lv_label_get_text(c), c) + pad_x(c);
      it.target = it.natural;
      it.frozen = false;
      used += it.natural;
    } else if (lv_obj_get_style_flex_grow(c, LV_PART_MAIN) > 0) {
      const int32_t min_w = lv_obj_get_style_min_width(c, LV_PART_MAIN);
      if (!LV_COORD_IS_PCT(min_w)) used += min_w;  // Grown items start at their minimum.
    } else {
      used += lv_obj_get_width(c);
    }
  }
  if (count == 0) return;
  if (visible > 1) used += lv_obj_get_style_pad_column(row, LV_PART_MAIN) * (visible - 1);
  const int32_t overflow = used - lv_obj_get_content_width(row);
  // Each text gives up a share of the overflow proportional to its natural
  // width; one that would go below its longest word stops there and the
  // rest is shared again among the others.
  int32_t left = overflow;
  for (;;) {
    int64_t basis = 0;
    for (int i = 0; i < count; ++i) {
      if (items[i].frozen) continue;
      items[i].target = items[i].natural;
      basis += items[i].natural;
    }
    if (left <= 0) break;
    if (basis <= 0) break;
    bool froze = false;
    for (int i = 0; i < count; ++i) {
      Item& it = items[i];
      if (it.frozen) continue;
      const int32_t cut = static_cast<int32_t>((static_cast<int64_t>(left) * it.natural + basis - 1) / basis);
      it.target = it.natural - cut;
      const int32_t floor = min_content_width(it.obj);
      if (it.target < floor) {
        it.target = floor < it.natural ? floor : it.natural;
        it.frozen = true;
        left -= it.natural - it.target;
        froze = true;
      }
    }
    if (!froze) break;
  }
  // Without overflow the labels keep the cap layout_children gives them.
  const bool definite = lv_obj_get_style_width(row, LV_PART_MAIN) != LV_SIZE_CONTENT;
  for (int i = 0; i < count; ++i) {
    const Item& it = items[i];
    const int32_t cap = overflow > 0 && it.target < it.natural ? it.target : (definite ? lv_pct(100) : LV_COORD_MAX);
    if (lv_obj_get_style_max_width(it.obj, LV_PART_MAIN) != cap) lv_obj_set_style_max_width(it.obj, cap, 0);
  }
}

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
    } else if (kind_ == Kind::Row) {
      lv_obj_add_event_cb(obj_, shrink_row_text, LV_EVENT_LAYOUT_CHANGED, nullptr);
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

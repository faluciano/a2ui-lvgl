// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Text (Markdown subset, see markdown.h) and Icon.
//
// Text renders plain content as an lv_label and switches to an lv_spangroup
// only when the Markdown needs several fonts or blocks. Neither sets a
// colour: both inherit the surrounding text colour (a primary button's
// contrasting colour reaches its label this way). The caption variant is
// the one exception: smaller and muted, as the catalog guide suggests.

#include <cstring>

#include "a2ui_lvgl/basic_catalog.h"
#include "a2ui_lvgl/markdown.h"
#include "basic_components.h"

namespace a2ui_lvgl {
namespace basic {

namespace {

namespace md = markdown;

class Text : public ComponentInstance {
 public:
  lv_obj_t* mount(NodeView& view, lv_obj_t* parent) override {
    parent_ = parent;
    obj_ = make_label(view);
    return obj_;
  }

  void update(NodeView& view) override {
    const std::string text = view.string_prop("text");
    const std::string variant = view.string_prop("variant", "body");
    if (initialized_ && text == source_ && variant == variant_) return;
    initialized_ = true;
    source_ = text;
    const bool variant_changed = variant != variant_;
    variant_ = variant;

    md::Document doc = md::parse(text);
    const Theme& t = view.theme();
    // A lone heading of plain runs still fits in one label (with the heading font).
    const bool lone_heading = doc.blocks.size() == 1 && doc.blocks[0].kind == md::Block::Kind::Heading &&
                              plain_runs(doc.blocks[0]);
    const bool use_spans = !(doc.is_plain() || lone_heading);

    if (use_spans != is_spans_) {
      lv_obj_t* replacement = use_spans ? make_spans(view) : make_label(view);
      view.swap_obj(replacement);
      obj_ = replacement;
      is_spans_ = use_spans;
      markers_.clear();
    } else if (variant_changed) {
      apply_variant(view, obj_);
    }

    if (!use_spans) {
      const lv_font_t* font = lone_heading ? heading_font(t, doc.blocks[0].level) : nullptr;
      if (font) {
        lv_obj_set_style_text_font(obj_, font, 0);
      } else {
        lv_obj_remove_local_style_prop(obj_, LV_STYLE_TEXT_FONT, 0);
      }
      set_label_text(obj_, doc.blocks.empty() ? std::string() : doc.plain_text());
      return;
    }
    fill_spans(view, doc);
  }

  void describe(const NodeView&, a2ui::Json* out) const override { out->set("text", a2ui::Json(visible_text())); }

  void infer_accessibility(const NodeView&, Accessibility* out) const override {
    out->role = "text";
    out->label = visible_text();
  }

  /** Visible text, list markers excluded, blocks separated by "\n". */
  std::string visible_text() const {
    if (!obj_) return {};
    if (!is_spans_) return lv_label_get_text(obj_);
    std::string out;
    const uint32_t n = lv_spangroup_get_span_count(obj_);
    for (uint32_t i = 0; i < n; ++i) {
      lv_span_t* span = lv_spangroup_get_child(obj_, static_cast<int32_t>(i));
      if (!span || is_marker(span)) continue;
      const char* t = lv_span_get_text(span);
      if (t) out += t;
    }
    return out;
  }

 private:
  static bool plain_runs(const md::Block& b) {
    for (const auto& r : b.runs) {
      if (r.style != md::kPlain) return false;
    }
    return true;
  }

  static const lv_font_t* heading_font(const Theme& t, int level) {
    if (level < 1 || level > 6) return nullptr;
    return t.heading[level - 1];
  }

  bool is_marker(lv_span_t* span) const {
    for (lv_span_t* m : markers_) {
      if (m == span) return true;
    }
    return false;
  }

  void apply_variant(NodeView& view, lv_obj_t* obj) {
    const Styles& s = styles(view);
    lv_obj_remove_style(obj, const_cast<lv_style_t*>(&s.caption), 0);
    if (variant_ == "caption") lv_obj_add_style(obj, const_cast<lv_style_t*>(&s.caption), 0);
  }

  lv_obj_t* make_label(NodeView& view) {
    const Styles& s = styles(view);
    lv_obj_t* label = lv_label_create(parent_);
    lv_obj_add_style(label, const_cast<lv_style_t*>(&s.leaf), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(label, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(label, lv_pct(100), 0);
    lv_label_set_text_static(label, "");
    apply_variant(view, label);
    copy_layout(obj_, label);
    return label;
  }

  lv_obj_t* make_spans(NodeView& view) {
    const Styles& s = styles(view);
    lv_obj_t* spans = lv_spangroup_create(parent_);
    lv_obj_add_style(spans, const_cast<lv_style_t*>(&s.leaf), 0);
    lv_obj_set_width(spans, lv_pct(100));
    lv_obj_set_height(spans, LV_SIZE_CONTENT);
    apply_variant(view, spans);
    copy_layout(obj_, spans);
    // A span group needs a definite width to wrap.
    if (lv_obj_get_style_width(spans, LV_PART_MAIN) == LV_SIZE_CONTENT) lv_obj_set_width(spans, lv_pct(100));
    return spans;
  }

  /** Carries what the parent's layout set on the old object over to its replacement. */
  void copy_layout(lv_obj_t* from, lv_obj_t* to) {
    if (!from || lv_obj_get_parent(from) != parent_) return;
    lv_obj_set_width(to, lv_obj_get_style_width(from, LV_PART_MAIN));
    lv_obj_set_flex_grow(to, lv_obj_get_style_flex_grow(from, LV_PART_MAIN));
    lv_obj_set_style_max_width(to, lv_obj_get_style_max_width(from, LV_PART_MAIN), 0);
    lv_obj_set_style_text_align(to, lv_obj_get_style_text_align(from, LV_PART_MAIN), 0);
  }

  lv_span_t* add_span(const std::string& text, const lv_font_t* font) {
    lv_span_t* span = lv_spangroup_add_span(obj_);
    lv_span_set_text(span, text.c_str());
    if (font) lv_style_set_text_font(lv_span_get_style(span), font);
    return span;
  }

  void fill_spans(NodeView& view, const md::Document& doc) {
    const Theme& t = view.theme();
    while (lv_spangroup_get_span_count(obj_) > 0) {
      lv_spangroup_delete_span(obj_, lv_spangroup_get_child(obj_, 0));
    }
    markers_.clear();
    for (size_t i = 0; i < doc.blocks.size(); ++i) {
      const md::Block& b = doc.blocks[i];
      if (i > 0) add_span("\n", nullptr);
      const lv_font_t* block_font = nullptr;
      if (b.kind == md::Block::Kind::Heading) block_font = heading_font(t, b.level);
      if (b.kind == md::Block::Kind::CodeBlock) block_font = t.code;
      if (b.kind == md::Block::Kind::ListItem) {
        std::string marker(static_cast<size_t>(b.level) * 2, ' ');
        marker += b.marker;
        marker += ' ';
        markers_.push_back(add_span(marker, nullptr));
      } else if (b.kind == md::Block::Kind::Quote) {
        markers_.push_back(add_span("  ", nullptr));
      }
      for (const auto& r : b.runs) {
        const lv_font_t* font = block_font;
        if (!font) {
          if (r.style & md::kCode) {
            font = t.code;
          } else if ((r.style & md::kStrong) && t.bold) {
            font = t.bold;
          } else if ((r.style & md::kEmphasis) && t.italic) {
            font = t.italic;
          }
        }
        add_span(r.text, font);
      }
    }
    lv_spangroup_refresh(obj_);
  }

  lv_obj_t* parent_ = nullptr;
  lv_obj_t* obj_ = nullptr;
  bool is_spans_ = false;
  bool initialized_ = false;
  std::string source_;
  std::string variant_ = "body";
  std::vector<lv_span_t*> markers_;
};

struct IconEntry {
  const char* name;
  const char* glyph;
};

// Basic catalog icon names onto the LV_SYMBOL_* glyphs of the built-in
// fonts; names without a symbol get a short ASCII stand-in.
constexpr IconEntry kIcons[] = {
    {"accountCircle", "@"},
    {"add", LV_SYMBOL_PLUS},
    {"arrowBack", LV_SYMBOL_LEFT},
    {"arrowForward", LV_SYMBOL_RIGHT},
    {"attachFile", LV_SYMBOL_FILE},
    {"calendarToday", LV_SYMBOL_LIST},
    {"call", LV_SYMBOL_CALL},
    {"camera", LV_SYMBOL_IMAGE},
    {"check", LV_SYMBOL_OK},
    {"close", LV_SYMBOL_CLOSE},
    {"delete", LV_SYMBOL_TRASH},
    {"download", LV_SYMBOL_DOWNLOAD},
    {"edit", LV_SYMBOL_EDIT},
    {"event", LV_SYMBOL_LIST},
    {"error", LV_SYMBOL_WARNING},
    {"fastForward", LV_SYMBOL_NEXT},
    {"favorite", LV_SYMBOL_TINT},
    {"favoriteOff", LV_SYMBOL_TINT},
    {"folder", LV_SYMBOL_DIRECTORY},
    {"help", "?"},
    {"home", LV_SYMBOL_HOME},
    {"info", "i"},
    {"locationOn", LV_SYMBOL_GPS},
    {"lock", LV_SYMBOL_EYE_CLOSE},
    {"lockOpen", LV_SYMBOL_EYE_OPEN},
    {"mail", LV_SYMBOL_ENVELOPE},
    {"menu", LV_SYMBOL_BARS},
    {"moreVert", ":"},
    {"moreHoriz", "..."},
    {"notificationsOff", LV_SYMBOL_BELL},
    {"notifications", LV_SYMBOL_BELL},
    {"pause", LV_SYMBOL_PAUSE},
    {"payment", "$"},
    {"person", "@"},
    {"phone", LV_SYMBOL_CALL},
    {"photo", LV_SYMBOL_IMAGE},
    {"play", LV_SYMBOL_PLAY},
    {"print", LV_SYMBOL_FILE},
    {"refresh", LV_SYMBOL_REFRESH},
    {"rewind", LV_SYMBOL_PREV},
    {"search", "?"},
    {"send", LV_SYMBOL_UPLOAD},
    {"settings", LV_SYMBOL_SETTINGS},
    {"share", LV_SYMBOL_UPLOAD},
    {"shoppingCart", "$"},
    {"skipNext", LV_SYMBOL_NEXT},
    {"skipPrevious", LV_SYMBOL_PREV},
    {"star", "*"},
    {"starHalf", "*"},
    {"starOff", "*"},
    {"stop", LV_SYMBOL_STOP},
    {"upload", LV_SYMBOL_UPLOAD},
    {"visibility", LV_SYMBOL_EYE_OPEN},
    {"visibilityOff", LV_SYMBOL_EYE_CLOSE},
    {"volumeDown", LV_SYMBOL_VOLUME_MID},
    {"volumeMute", LV_SYMBOL_MUTE},
    {"volumeOff", LV_SYMBOL_MUTE},
    {"volumeUp", LV_SYMBOL_VOLUME_MAX},
    {"warning", LV_SYMBOL_WARNING},
};

constexpr const char* kFallbackGlyph = LV_SYMBOL_BULLET;

class Icon : public ComponentInstance {
 public:
  lv_obj_t* mount(NodeView& view, lv_obj_t* parent) override {
    const Styles& s = styles(view);
    obj_ = lv_label_create(parent);
    lv_obj_add_style(obj_, const_cast<lv_style_t*>(&s.leaf), 0);
    if (view.theme().icon) lv_obj_set_style_text_font(obj_, view.theme().icon, 0);
    lv_label_set_text_static(obj_, "");
    return obj_;
  }

  void update(NodeView& view) override {
    const a2ui::Json& v = view.value("name");
    name_ = v.is_string() ? v.as_string() : std::string();
    const char* glyph = nullptr;
    if (!name_.empty() && view.surface().services().icon_glyph) glyph = view.surface().services().icon_glyph(name_);
    if (!glyph || !*glyph) glyph = name_.empty() ? kFallbackGlyph : basic_icon_glyph(name_);
    set_label_text(obj_, glyph);
  }

  void describe(const NodeView&, a2ui::Json* out) const override { out->set("name", a2ui::Json(name_)); }

  void infer_accessibility(const NodeView&, Accessibility* out) const override {
    out->role = "img";
    out->label = name_;
  }

 private:
  lv_obj_t* obj_ = nullptr;
  std::string name_;
};

}  // namespace

std::unique_ptr<ComponentInstance> make_text() { return std::make_unique<Text>(); }
std::unique_ptr<ComponentInstance> make_icon() { return std::make_unique<Icon>(); }

}  // namespace basic

const char* basic_icon_glyph(const std::string& name) {
  for (const auto& e : basic::kIcons) {
    if (name == e.name) return e.glyph;
  }
  return basic::kFallbackGlyph;
}

}  // namespace a2ui_lvgl

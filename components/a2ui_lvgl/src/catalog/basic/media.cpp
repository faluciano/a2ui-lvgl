// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Image, Video and AudioPlayer.
//
// Image validates its URL with the catalog's media URL rules
// (basic_catalog::is_allowed_media_url) and asks the application's
// ImageLoader for pixels; until they arrive, or for a rejected URL, it shows
// a placeholder box sized by the variant. Video and AudioPlayer render
// nothing: there is no media pipeline on a microcontroller. They keep their
// view (and so their identity and accessibility) without an object.

#include "a2ui/basic_catalog.h"
#include "basic_components.h"

namespace a2ui_lvgl {
namespace basic {

namespace {

using a2ui::Json;

lv_image_align_t fit_to_align(const std::string& fit) {
  if (fit == "contain") return LV_IMAGE_ALIGN_CONTAIN;
  if (fit == "cover") return LV_IMAGE_ALIGN_COVER;
  if (fit == "none") return LV_IMAGE_ALIGN_CENTER;
  if (fit == "scaleDown") return LV_IMAGE_ALIGN_CONTAIN_DOWNSCALE;
  return LV_IMAGE_ALIGN_STRETCH;  // "fill" (the default)
}

class Image : public ComponentInstance {
 public:
  ~Image() override { *alive_ = false; }

  lv_obj_t* mount(NodeView& view, lv_obj_t* parent) override {
    view_ = &view;
    const Styles& s = styles(view);
    box_ = lv_obj_create(parent);
    lv_obj_remove_style_all(box_);
    lv_obj_add_style(box_, const_cast<lv_style_t*>(&s.placeholder_box), 0);
    lv_obj_add_style(box_, const_cast<lv_style_t*>(&s.leaf), 0);
    lv_obj_set_clickable(box_, false);
    lv_obj_set_scrollable(box_, false);
    placeholder_ = lv_label_create(box_);
    lv_label_set_text_static(placeholder_, LV_SYMBOL_IMAGE);
    lv_obj_center(placeholder_);
    image_ = lv_image_create(box_);
    lv_obj_set_size(image_, lv_pct(100), lv_pct(100));
    lv_obj_set_hidden(image_, true);
    return box_;
  }

  void update(NodeView& view) override {
    const Theme& t = view.theme();
    const std::string variant = view.string_prop("variant", "mediumFeature");
    const std::string fit = view.string_prop("fit", "fill");
    if (variant != variant_) {
      variant_ = variant;
      apply_variant(t);
    }
    if (fit != fit_) {
      fit_ = fit;
      lv_image_set_inner_align(image_, fit_to_align(fit));
    }
    std::string url = view.string_prop("url");
    if (!a2ui::basic_catalog::is_allowed_media_url(url)) url.clear();
    if (url == url_) return;
    url_ = url;
    request_.reset();
    lv_obj_set_hidden(image_, true);
    lv_obj_set_hidden(placeholder_, false);
    loaded_ = false;
    ImageLoader* loader = view.surface().services().image_loader;
    if (url_.empty() || !loader) return;
    ImageRequest req;
    req.url = url_;
    req.variant = variant_;
    req.fit = fit_;
    req.width = lv_obj_get_style_width(box_, LV_PART_MAIN) > 0 && !LV_COORD_IS_PCT(lv_obj_get_style_width(box_, LV_PART_MAIN))
                    ? lv_obj_get_style_width(box_, LV_PART_MAIN)
                    : 0;
    req.height = lv_obj_get_style_height(box_, LV_PART_MAIN) > 0 ? lv_obj_get_style_height(box_, LV_PART_MAIN) : 0;
    std::shared_ptr<bool> alive = alive_;
    const uint32_t generation = ++generation_;
    Image* self = this;
    request_ = loader->load(req, [alive, generation, self](const void* src) {
      if (!*alive || generation != self->generation_) return;
      self->show(src);
    });
  }

  void unmount(NodeView&) override { request_.reset(); }

  void describe(const NodeView&, Json* out) const override {
    out->set("url", Json(url_));
    out->set("variant", Json(variant_));
    out->set("loaded", Json(loaded_));
  }

  void infer_accessibility(const NodeView& view, Accessibility* out) const override {
    out->role = "img";
    out->label = view.string_prop("description");
  }

 private:
  void show(const void* src) {
    if (!src) return;
    lv_image_set_src(image_, src);
    lv_obj_set_hidden(image_, false);
    lv_obj_set_hidden(placeholder_, true);
    loaded_ = true;
  }

  void apply_variant(const Theme& t) {
    int32_t w = lv_pct(100), h = t.image_medium * 3 / 4, max_w = t.image_large;
    int32_t radius = t.control_radius;
    if (variant_ == "icon") {
      w = h = max_w = t.image_icon;
      radius = 0;
    } else if (variant_ == "avatar") {
      w = h = max_w = t.image_avatar;
      radius = LV_RADIUS_CIRCLE;
    } else if (variant_ == "smallFeature") {
      w = h = max_w = t.image_small;
    } else if (variant_ == "largeFeature") {
      h = t.image_large;
      max_w = LV_COORD_MAX;
    } else if (variant_ == "header") {
      h = t.image_header;
      max_w = LV_COORD_MAX;
      radius = 0;
    }
    lv_obj_set_size(box_, w, h);
    lv_obj_set_style_max_width(box_, max_w, 0);
    // A full-width image in a Row sizes to content; keep it visible there.
    lv_obj_set_style_min_width(box_, LV_COORD_IS_PCT(w) ? t.image_small : 0, 0);
    lv_obj_set_style_radius(box_, radius, 0);
  }

  NodeView* view_ = nullptr;
  lv_obj_t* box_ = nullptr;
  lv_obj_t* placeholder_ = nullptr;
  lv_obj_t* image_ = nullptr;
  std::string variant_;
  std::string fit_;
  std::string url_;
  bool loaded_ = false;
  uint32_t generation_ = 0;
  std::shared_ptr<void> request_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

/** Video / AudioPlayer: no object; reports its URL for inspection. */
class NoMedia : public ComponentInstance {
 public:
  explicit NoMedia(bool video) : video_(video) {}
  lv_obj_t* mount(NodeView&, lv_obj_t*) override { return nullptr; }
  void update(NodeView&) override {}
  void describe(const NodeView& view, Json* out) const override {
    std::string url = view.string_prop("url");
    if (!a2ui::basic_catalog::is_allowed_media_url(url)) url.clear();
    out->set("url", Json(url));
  }
  void infer_accessibility(const NodeView& view, Accessibility* out) const override {
    out->role = video_ ? "video" : "audio";
    out->label = view.string_prop("description");
  }

 private:
  bool video_;
};

}  // namespace

std::unique_ptr<ComponentInstance> make_image() { return std::make_unique<Image>(); }
std::unique_ptr<ComponentInstance> make_video() { return std::make_unique<NoMedia>(true); }
std::unique_ptr<ComponentInstance> make_audio_player() { return std::make_unique<NoMedia>(false); }

}  // namespace basic
}  // namespace a2ui_lvgl

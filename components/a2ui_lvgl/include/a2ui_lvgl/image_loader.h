// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Image fetching is the application's job: a microcontroller may load from
// HTTP(S), a cache on flash, or a table of built-in images. The Image
// component validates the URL (basic_catalog::is_allowed_media_url, the
// catalog's media URL rules) and then asks the ImageLoader for pixels. Until
// the loader answers (or without a loader) it shows a placeholder box sized
// by the variant.

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "lvgl.h"

namespace a2ui_lvgl {

struct ImageRequest {
  /** The validated URL (http(s), data: or relative). */
  std::string url;
  /** "icon", "avatar", "smallFeature", "mediumFeature", "largeFeature" or "header". */
  std::string variant;
  /** "contain", "cover", "fill", "none" or "scaleDown". */
  std::string fit;
  /** The box the image is drawn in (0 when it follows the container width). */
  int32_t width = 0;
  int32_t height = 0;
};

/**
 * Delivers a loaded image. `src` is anything lv_image_set_src accepts (an
 * lv_image_dsc_t*, a file system path such as "S:/img.png" when a decoder is
 * enabled, or an LV_SYMBOL_* string) and must stay valid while the image is
 * shown. Null reports failure (the placeholder stays). Call on the LVGL task;
 * calling after the component went away, or more than once, is harmless.
 */
using ImageDone = std::function<void(const void* src)>;

class ImageLoader {
 public:
  virtual ~ImageLoader() = default;
  /**
   * Starts loading. May call `done` synchronously. The returned handle is
   * dropped when the request is no longer wanted (the URL changed or the
   * view unmounted); a loader can use its destruction to cancel.
   */
  virtual std::shared_ptr<void> load(const ImageRequest& request, ImageDone done) = 0;
};

}  // namespace a2ui_lvgl

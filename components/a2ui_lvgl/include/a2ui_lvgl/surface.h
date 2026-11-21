// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// The surface host (framework adapter blueprint §4.1).
//
// Surface takes a SurfaceModel and an LVGL parent. It creates a container
// object under the parent, owns a NodeResolver for the model, renders the
// resolver's root node through NodeView, and tears everything down when it
// is destroyed, when the model is disposed (deleteSurface), or when the
// container (or an ancestor) is deleted from outside. It does not process
// messages, negotiate capabilities or run RPC; the MessageProcessor does.
//
// Actions and errors are not routed here: subscribe to the model's
// on_action() / on_error() (or the SurfaceGroupModel's on_action()).

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "a2ui/node_resolver.h"
#include "a2ui/surface_model.h"
#include "a2ui_lvgl/component.h"
#include "a2ui_lvgl/image_loader.h"
#include "a2ui_lvgl/theme.h"
#include "lvgl.h"

namespace a2ui_lvgl {

/** Blueprint §4.4 fallback info. */
struct PendingInfo {
  /** The id of the component that has not arrived (the root id before the root arrives). */
  std::string component_id;
  /** Type of the component whose child this is; empty for the root. */
  std::string parent_component_type;
};

struct ErrorInfo {
  std::string component_id;
  std::string parent_component_type;
  /** "unknown-type", "cyclic", or "unimplemented" (resolved, but no implementation registered). */
  std::string type;
  /** The declared type, for "unknown-type" and "unimplemented". */
  std::string component_type;
};

/**
 * Optional UI for nodes that cannot render (yet). A builder creates its
 * objects under `parent` and returns the root, or returns null to render
 * nothing (no layout footprint). Builders must not report errors; the
 * resolver already has.
 */
struct Fallbacks {
  std::function<lv_obj_t*(lv_obj_t* parent, const PendingInfo& info)> pending;
  std::function<lv_obj_t*(lv_obj_t* parent, const ErrorInfo& info)> error;
};

/** Host hooks the components use. All optional. */
struct Services {
  /** Fetches images for Image (see image_loader.h). Not owned; must outlive the surface. */
  ImageLoader* image_loader = nullptr;
  /**
   * A text input gained (true) or lost (false) focus: the place to show or
   * hide an on-screen lv_keyboard (lv_keyboard_set_textarea(kb, textarea)).
   */
  std::function<void(lv_obj_t* textarea, bool focused)> on_text_focus;
  /**
   * A live region (`@accessibility.live` polite/assertive) changed what it
   * says: hand the text to a speech or announcement layer.
   */
  std::function<void(const std::string& text, bool assertive)> announce;
  /** Maps an Icon name to a glyph string (UTF-8 for theme.icon); null/empty uses the built-in table. */
  std::function<const char*(const std::string& name)> icon_glyph;
};

struct SurfaceConfig {
  /**
   * Component implementations. Not owned; must outlive the surface. The
   * adapter core does not depend on the basic catalog: pass
   * &a2ui_lvgl::basic_registry() or a registry of your own. Without one,
   * every node renders the error fallback ("unimplemented").
   */
  const ComponentRegistry* registry = nullptr;
  /** Copied. Defaults to Theme::light(). */
  const Theme* theme = nullptr;
  Fallbacks fallbacks;
  Services services;
  /**
   * Size the container to the parent (100% x 100%, scrolls vertically);
   * false sizes it to its content, full width.
   */
  bool fill_parent = true;
  a2ui::NodeResolverOptions resolver_options;
};

class Surface {
 public:
  Surface(a2ui::SurfaceModel& model, lv_obj_t* parent, SurfaceConfig config = {});
  ~Surface();
  Surface(const Surface&) = delete;
  Surface& operator=(const Surface&) = delete;

  /** The surface container; null once unmounted. */
  lv_obj_t* obj() const { return container_; }
  /** False once unmounted (destroyed model, deleted container, or unmount()). */
  bool mounted() const { return container_ != nullptr; }
  /** The model; null once unmounted (the model may then be disposed at any time). */
  a2ui::SurfaceModel* model() const { return model_; }
  a2ui::NodeResolver* resolver() const { return resolver_.get(); }
  /** The root view; null before the root component arrives. */
  NodeView* root_view() const { return root_.get(); }

  const Theme& theme() const { return theme_; }
  const SurfaceConfig& config() const { return config_; }
  const Services& services() const { return config_.services; }

  /** Resolver subscriptions plus the props subscriptions of every view (0 once unmounted). */
  size_t active_subscription_count() const;

  /** Depth-first search for the first view of `component_id`. */
  NodeView* find_view(std::string_view component_id) const;

  /** Tears the views down, disposes the resolver and deletes the container. Idempotent. */
  void unmount();

  /**
   * A per-surface slot for implementation state shared by all views of the
   * surface (style sheets, caches), keyed by the address of a tag object.
   * Released after every object of the surface has been deleted.
   */
  std::shared_ptr<void>& resource(const void* key);

  // --- Used by NodeView ------------------------------------------------------

  /** The implementation for a resolved node (null when none is registered). */
  const ComponentImplementation* implementation_for(const a2ui::ComponentNode& node) const;
  /** Monotonic counter for view identity (mount serials). */
  uint32_t next_serial() { return ++serial_; }

 private:
  void on_root(const a2ui::NodePtr& root);
  void show_root_pending();
  void clear_root_pending();
  void teardown(bool objects_alive);
  static void container_event_cb(lv_event_t* e);

  a2ui::SurfaceModel* model_;
  SurfaceConfig config_;
  Theme theme_;
  lv_obj_t* container_ = nullptr;
  lv_obj_t* pending_obj_ = nullptr;
  std::unique_ptr<a2ui::NodeResolver> resolver_;
  std::unique_ptr<NodeView> root_;
  a2ui::Subscription root_sub_;
  a2ui::Subscription disposed_sub_;
  std::vector<std::pair<const void*, std::shared_ptr<void>>> resources_;
  uint32_t serial_ = 0;
};

}  // namespace a2ui_lvgl

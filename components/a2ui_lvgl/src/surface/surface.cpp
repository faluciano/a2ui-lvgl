// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Surface host (blueprint §4.1, §7): owns the NodeResolver, renders its root
// node, and tears down on destruction, model disposal or container deletion.

#include "a2ui_lvgl/surface.h"

namespace a2ui_lvgl {

using a2ui::NodePtr;

namespace {

void null_pointer_on_delete(lv_event_t* e) {
  auto** slot = static_cast<lv_obj_t**>(lv_event_get_user_data(e));
  if (*slot == lv_event_get_target(e)) *slot = nullptr;
}

NodeView* find_in(NodeView* v, std::string_view id) {
  if (!v) return nullptr;
  if (v->node().component_id() == id) return v;
  for (NodeView* c : v->children()) {
    if (NodeView* f = find_in(c, id)) return f;
  }
  return nullptr;
}

}  // namespace

Surface::Surface(a2ui::SurfaceModel& model, lv_obj_t* parent, SurfaceConfig config)
    : model_(&model), config_(std::move(config)), theme_(config_.theme ? *config_.theme : Theme::light()) {
  config_.theme = nullptr;  // Copied into theme_.
  container_ = lv_obj_create(parent);
  lv_obj_remove_style_all(container_);
  if (config_.fill_parent) {
    lv_obj_set_size(container_, lv_pct(100), lv_pct(100));
    lv_obj_set_scroll_dir(container_, LV_DIR_VER);
  } else {
    lv_obj_set_size(container_, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_scrollable(container_, false);
  }
  lv_obj_set_clickable(container_, false);
  lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_bg_color(container_, theme_.background, 0);
  lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(container_, theme_.on_background, 0);
  if (theme_.body) lv_obj_set_style_text_font(container_, theme_.body, 0);
  lv_obj_add_event_cb(container_, &Surface::container_event_cb, LV_EVENT_DELETE, this);

  resolver_ = std::make_unique<a2ui::NodeResolver>(model, config_.resolver_options);
  disposed_sub_ = model.on_disposed().subscribe([this] {
    // deleteSurface: the resolver has torn its tree down already (it
    // subscribed first); unmount the view and forget the model.
    teardown(true);
  });
  root_sub_ = resolver_->root_node().subscribe([this](const NodePtr& root) { on_root(root); });
  on_root(resolver_->root_node().value());
}

Surface::~Surface() {
  teardown(true);
  resources_.clear();
}

void Surface::unmount() { teardown(true); }

void Surface::on_root(const NodePtr& root) {
  if (!container_) return;
  if (!root) {
    root_.reset();
    show_root_pending();
    return;
  }
  clear_root_pending();
  if (root_ && root_->instance_id() == root->instance_id()) {
    root_->set_node(root);
  } else {
    root_.reset();
    root_ = std::make_unique<NodeView>(*this, nullptr, root);
    root_->attach(container_);
  }
  // The container stacks its one child like a Column with align=stretch.
  if (lv_obj_t* o = root_->obj()) {
    if (lv_obj_get_style_width(o, LV_PART_MAIN) != lv_pct(100)) lv_obj_set_width(o, lv_pct(100));
  }
}

void Surface::show_root_pending() {
  if (pending_obj_ || !config_.fallbacks.pending || !container_ || !model_) return;
  PendingInfo info{model_->root_id(), {}};
  pending_obj_ = config_.fallbacks.pending(container_, info);
  if (pending_obj_) lv_obj_add_event_cb(pending_obj_, null_pointer_on_delete, LV_EVENT_DELETE, &pending_obj_);
}

void Surface::clear_root_pending() {
  if (!pending_obj_) return;
  lv_obj_t* o = pending_obj_;
  pending_obj_ = nullptr;
  lv_obj_remove_event_cb_with_user_data(o, null_pointer_on_delete, &pending_obj_);
  lv_obj_delete(o);
}

void Surface::teardown(bool objects_alive) {
  root_sub_.unsubscribe();
  disposed_sub_.unsubscribe();
  model_ = nullptr;  // No longer tracked: it may be destroyed at any time now.
  if (root_) {
    if (!objects_alive) root_->detach_objects();
    root_.reset();
  }
  if (resolver_) {
    resolver_->dispose();
    resolver_.reset();
  }
  if (pending_obj_) {
    if (objects_alive) {
      clear_root_pending();
    } else {
      lv_obj_remove_event_cb_with_user_data(pending_obj_, null_pointer_on_delete, &pending_obj_);
      pending_obj_ = nullptr;
    }
  }
  if (container_) {
    lv_obj_t* c = container_;
    container_ = nullptr;
    if (objects_alive) {
      lv_obj_remove_event_cb_with_user_data(c, &Surface::container_event_cb, this);
      lv_obj_delete(c);
    }
  }
  // Resources (style sheets) stay until destruction: objects being deleted
  // from outside still reference them until LVGL finishes.
}

void Surface::container_event_cb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
  auto* self = static_cast<Surface*>(lv_event_get_user_data(e));
  self->teardown(false);
}

size_t Surface::active_subscription_count() const {
  size_t n = 0;
  if (resolver_) n += resolver_->active_subscription_count();
  if (root_) n += root_->subscription_count();
  return n;
}

NodeView* Surface::find_view(std::string_view component_id) const { return find_in(root_.get(), component_id); }

std::shared_ptr<void>& Surface::resource(const void* key) {
  for (auto& r : resources_) {
    if (r.first == key) return r.second;
  }
  resources_.emplace_back(key, nullptr);
  return resources_.back().second;
}

const ComponentImplementation* Surface::implementation_for(const a2ui::ComponentNode& node) const {
  if (!config_.registry) return nullptr;
  std::string_view catalog_id;
  if (model_ && node.impl()) {
    const a2ui::Catalog* def = model_->default_catalog();
    if (def && def->find_component(node.type()) == node.impl()) {
      catalog_id = def->id();
    } else {
      for (const auto& c : model_->available_catalogs()) {
        if (c.second && c.second->find_component(node.type()) == node.impl()) {
          catalog_id = c.second->id();
          break;
        }
      }
    }
  }
  return config_.registry->find(catalog_id, node.type());
}

}  // namespace a2ui_lvgl

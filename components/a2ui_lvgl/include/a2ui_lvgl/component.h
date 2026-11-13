// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Component implementations and the node dispatcher (framework adapter
// blueprint §4.2 "Form 4: Stateful / Imperative Instances" and §6).
//
// LVGL objects are long-lived and mutable, so a component implementation is
// a factory of ComponentInstance objects: mount() creates the widget once,
// update() applies the node's current props whenever they change, unmount()
// releases what lives outside the widget's own object tree.
//
// NodeView is the adapter's per-node view: it keys its identity by the
// node's instance id, subscribes to the node's props, creates the instance
// the registry names for the node's type, renders nothing (or the surface's
// fallback) for pending, unknown-type and cyclic nodes, keeps child views
// keyed by instance id (moving their objects on a reorder, recreating a
// view only when the component's type changes) and computes accessibility
// attributes for every component in one place.
//
// Threading: everything runs on the LVGL task. Object lifetime: a view's
// root object belongs to the view; deleting it (or an ancestor) from
// outside is safe (the view notices LV_EVENT_DELETE and goes inert).

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "a2ui/component_node.h"
#include "a2ui/events.h"
#include "a2ui/json.h"
#include "a2ui_lvgl/accessibility.h"
#include "lvgl.h"

namespace a2ui_lvgl {

class NodeView;
class Surface;
struct Theme;

/** One mounted component (Form 4). Created per view; replaced when the component's type changes. */
class ComponentInstance {
 public:
  virtual ~ComponentInstance() = default;

  /**
   * Creates the widget under `parent` and returns its root object, which
   * becomes the view's object. Return null to render nothing (no layout
   * footprint). Called once, before the first update().
   */
  virtual lv_obj_t* mount(NodeView& view, lv_obj_t* parent) = 0;

  /**
   * Applies view.props(): after mount, when the props change, and when the
   * view moves to a replacement node of the same type. Containers place
   * their children here with NodeView::sync_slot().
   */
  virtual void update(NodeView& view) = 0;

  /**
   * Releases resources outside the root object's subtree (overlays on the
   * top layer, timers, image requests). Child views are already gone. The
   * root object is deleted by the view afterwards; view.obj() is null when
   * the objects were deleted from outside, in which case only out-of-tree
   * resources may be touched.
   */
  virtual void unmount(NodeView& view) {}

  /**
   * Fills inferred accessibility (role, label from visible text, checked
   * state). Explicit `@accessibility` values override these afterwards.
   */
  virtual void infer_accessibility(const NodeView& view, Accessibility* out) const {}

  /**
   * Reports what the widget currently shows, read back from the LVGL
   * objects ("text", "value", "url", "name", "axis", "hints", ...). Used by
   * tests and inspection tools; never by rendering.
   */
  virtual void describe(const NodeView& view, a2ui::Json* out) const {}

  /**
   * Automation seam: drives the native control as a user would (sets the
   * widget state and sends the LVGL event its handler listens to). Returns
   * false when the component takes no such input.
   */
  virtual bool simulate_input(NodeView& view, const a2ui::Json& value) { return false; }
  /** Automation seam: sends the click/tap this component reacts to. */
  virtual bool simulate_gesture(NodeView& view) { return false; }
};

/** Pairs a component type with an instance factory. */
struct ComponentImplementation {
  /** Component type name ("Button"). */
  std::string name;
  /** Catalog the entry applies to; empty for any catalog that declares `name`. */
  std::string catalog_id;
  std::function<std::unique_ptr<ComponentInstance>()> create;
};

/**
 * Implementations by (catalog id, type). Lookup tries the node's own
 * catalog first, then the catalog-agnostic entry, so one implementation can
 * serve every catalog declaring the type and an application can override a
 * single component (add() replaces an entry with the same key).
 */
class ComponentRegistry {
 public:
  void add(ComponentImplementation impl);
  const ComponentImplementation* find(std::string_view catalog_id, std::string_view name) const;
  size_t size() const { return entries_.size(); }

 private:
  std::vector<ComponentImplementation> entries_;
};

namespace detail {
template <class T, void (T::*Method)(lv_event_t*)>
void member_event_thunk(lv_event_t* e) {
  (static_cast<T*>(lv_event_get_user_data(e))->*Method)(e);
}
}  // namespace detail

/** lv_obj_add_event_cb calling `(self->*Method)(e)`. */
template <class T, void (T::*Method)(lv_event_t*)>
void add_event(lv_obj_t* obj, lv_event_code_t code, T* self) {
  lv_obj_add_event_cb(obj, &detail::member_event_thunk<T, Method>, code, self);
}

class NodeView {
 public:
  NodeView(Surface& surface, NodeView* parent, a2ui::NodePtr node);
  ~NodeView();
  NodeView(const NodeView&) = delete;
  NodeView& operator=(const NodeView&) = delete;

  // --- Node and props ------------------------------------------------------

  const std::string& instance_id() const { return instance_id_; }
  a2ui::ComponentNode& node() const { return *node_; }
  const a2ui::NodePtr& node_ptr() const { return node_; }
  const a2ui::NodeProps& props() const { return node_->props(); }
  const a2ui::PropValue* prop(std::string_view key) const { return node_->prop(key); }

  /** The value of a literal or binding prop; undefined when absent (or an action / node). */
  const a2ui::Json& value(std::string_view key) const;
  /** String value; numbers and booleans are stringified; `fallback` when absent or null. */
  std::string string_prop(std::string_view key, std::string_view fallback = {}) const;
  double number_prop(std::string_view key, double fallback) const;
  bool bool_prop(std::string_view key, bool fallback) const;
  /** Child nodes held by `key` (a single child or a child list), in order. */
  std::vector<a2ui::ComponentNode*> child_nodes(std::string_view key) const;
  a2ui::ComponentNode* child_node(std::string_view key) const;

  // --- Placement -------------------------------------------------------------

  Surface& surface() const { return *surface_; }
  const Theme& theme() const;
  NodeView* parent() const { return parent_; }
  /** The parent's component type ("" for the root): the fallback info's parentComponentType. */
  const std::string& parent_type() const { return parent_type_; }
  /** Root object; null while the view renders nothing. */
  lv_obj_t* obj() const { return obj_; }
  ComponentInstance* instance() const { return instance_.get(); }
  /** True when the view shows an application fallback rather than a component. */
  bool is_fallback() const { return fallback_; }
  /** Changes whenever the view creates a new instance or fallback (view identity for tests). */
  uint32_t mount_serial() const { return mount_serial_; }
  /**
   * For implementations that switch widget kind on update (Text: label <->
   * span group): makes `replacement` (created under the same parent) the
   * root object at the old one's position and deletes the old one.
   */
  void swap_obj(lv_obj_t* replacement);

  // --- Children --------------------------------------------------------------

  /**
   * Shows `nodes` as this view's children in slot `slot`, inside
   * `container`, in order, starting at child index `base_index` of the
   * container (objects before it belong to the implementation). Existing
   * child views are reused by instance id: moved, switched to the new node,
   * or recreated only when the type changed. Children that render nothing
   * get no object. Returns the slot's views in order.
   */
  const std::vector<NodeView*>& sync_slot(size_t slot, lv_obj_t* container,
                                          const std::vector<a2ui::ComponentNode*>& nodes,
                                          uint32_t base_index = 0);
  /** Destroys the child views of `slot`. */
  void clear_slot(size_t slot);
  /** The views of one slot (empty if never synced). */
  const std::vector<NodeView*>& slot_views(size_t slot) const;
  /** All child views, slot by slot, in order. */
  std::vector<NodeView*> children() const;

  // --- Accessibility and automation -----------------------------------------

  /** Inferred attributes overridden by the explicit `@accessibility` ones. */
  Accessibility accessibility() const;
  /** The implementation's describe() (empty object when none). */
  a2ui::Json describe() const;
  bool simulate_input(const a2ui::Json& value);
  bool simulate_gesture();

  // --- Used by Surface ---------------------------------------------------------

  /** Renders into `host` (the container the view's object lives in). */
  void attach(lv_obj_t* host);
  /** Moves the view to a replacement node for the same instance id. */
  void set_node(a2ui::NodePtr node);
  /** The surface container is being deleted: let go of every object without deleting it. */
  void detach_objects();
  /** Props subscriptions held by this view and its descendants. */
  size_t subscription_count() const;

 private:
  struct Slot {
    lv_obj_t* container = nullptr;
    std::vector<std::unique_ptr<NodeView>> owned;
    std::vector<NodeView*> views;
  };

  void render();
  void teardown(bool objects_alive);
  void set_obj(lv_obj_t* obj);
  void release_obj(bool delete_it);
  void on_props(const a2ui::NodeProps& props);
  void place_in_slot();
  static void obj_event_cb(lv_event_t* e);
  friend NodeView* view_of(lv_obj_t* obj);

  Surface* surface_;
  NodeView* parent_;
  std::string parent_type_;
  std::string instance_id_;
  a2ui::NodePtr node_;
  a2ui::Subscription props_sub_;
  std::unique_ptr<ComponentInstance> instance_;
  std::string instance_type_;
  const void* instance_impl_ = nullptr;
  lv_obj_t* host_ = nullptr;
  lv_obj_t* obj_ = nullptr;
  bool fallback_ = false;
  bool rendering_ = false;
  uint32_t mount_serial_ = 0;
  std::vector<Slot> slots_;
  std::string last_announced_;
};

}  // namespace a2ui_lvgl

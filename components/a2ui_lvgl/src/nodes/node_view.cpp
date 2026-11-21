// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// NodeView: the recursive node dispatcher (blueprint §6) and the one place
// accessibility attributes are computed (§3).

#include <cstdlib>
#include <unordered_map>

#include "a2ui/coerce.h"
#include "a2ui_lvgl/component.h"
#include "a2ui_lvgl/surface.h"

namespace a2ui_lvgl {

using a2ui::ComponentNode;
using a2ui::Json;
using a2ui::NodePtr;
using a2ui::NodeState;
using a2ui::PropValue;

namespace {

const Json& undefined_json() {
  static const Json kUndefined;
  return kUndefined;
}

const std::vector<NodeView*>& empty_views() {
  static const std::vector<NodeView*> kEmpty;
  return kEmpty;
}

void collect_nodes(const PropValue& v, std::vector<ComponentNode*>* out) {
  if (v.is_node()) {
    if (v.node()) out->push_back(v.node());
  } else if (v.is_array()) {
    for (const auto& item : v.items()) collect_nodes(item, out);
  }
}

std::string json_text(const Json& j) {
  if (j.is_string()) return j.as_string();
  if (j.is_number()) return a2ui::number_to_string(j.as_number());
  if (j.is_bool()) return j.as_bool() ? "true" : "false";
  return {};
}

}  // namespace

NodeView::NodeView(Surface& surface, NodeView* parent, NodePtr node)
    : surface_(&surface),
      parent_(parent),
      parent_type_(parent ? parent->node().type() : std::string()),
      instance_id_(node->instance_id()),
      node_(std::move(node)) {}

NodeView::~NodeView() { teardown(true); }

const Theme& NodeView::theme() const { return surface_->theme(); }

// --- Props helpers -------------------------------------------------------------

const Json& NodeView::value(std::string_view key) const {
  const PropValue* p = prop(key);
  if (!p || !(p->is_literal() || p->is_binding())) return undefined_json();
  return p->value();
}

std::string NodeView::string_prop(std::string_view key, std::string_view fallback) const {
  const Json& v = value(key);
  if (v.is_nullish()) return std::string(fallback);
  return json_text(v);
}

double NodeView::number_prop(std::string_view key, double fallback) const {
  const Json& v = value(key);
  if (v.is_number()) return v.as_number();
  if (v.is_string()) {
    const std::string& s = v.as_string();
    char* end = nullptr;
    double d = std::strtod(s.c_str(), &end);
    if (end && end != s.c_str() && *end == '\0') return d;
  }
  return fallback;
}

bool NodeView::bool_prop(std::string_view key, bool fallback) const {
  const Json& v = value(key);
  return v.is_bool() ? v.as_bool() : fallback;
}

std::vector<ComponentNode*> NodeView::child_nodes(std::string_view key) const {
  std::vector<ComponentNode*> out;
  if (const PropValue* p = prop(key)) collect_nodes(*p, &out);
  return out;
}

ComponentNode* NodeView::child_node(std::string_view key) const {
  const PropValue* p = prop(key);
  return p && p->is_node() ? p->node() : nullptr;
}

// --- Rendering -------------------------------------------------------------------

void NodeView::attach(lv_obj_t* host) {
  host_ = host;
  render();
}

void NodeView::render() {
  rendering_ = true;
  fallback_ = false;
  node_->set_user_data(this);
  props_sub_ = node_->on_props_changed([this](const a2ui::NodeProps& p) { on_props(p); });

  const Fallbacks& fallbacks = surface_->config().fallbacks;
  if (node_->state() == NodeState::Resolved) {
    if (const ComponentImplementation* impl = surface_->implementation_for(*node_)) {
      instance_ = impl->create();
      instance_type_ = node_->type();
      instance_impl_ = impl;
      mount_serial_ = surface_->next_serial();
      set_obj(instance_->mount(*this, host_));
      instance_->update(*this);
      Accessibility a = accessibility();
      last_announced_ = a.label;
      rendering_ = false;
      return;
    }
    LV_LOG_WARN("a2ui: no implementation for component type '%s'", node_->type().c_str());
    if (fallbacks.error) {
      ErrorInfo info{node_->component_id(), parent_type_, "unimplemented", node_->type()};
      set_obj(fallbacks.error(host_, info));
    }
  } else if (node_->state() == NodeState::Pending) {
    if (fallbacks.pending) {
      PendingInfo info{node_->component_id(), parent_type_};
      set_obj(fallbacks.pending(host_, info));
    }
  } else if (fallbacks.error) {
    ErrorInfo info{node_->component_id(), parent_type_, a2ui::node_state_name(node_->state()), {}};
    if (node_->state() == NodeState::UnknownType) info.component_type = node_->type();
    set_obj(fallbacks.error(host_, info));
  }
  if (obj_) {
    fallback_ = true;
    mount_serial_ = surface_->next_serial();
  }
  rendering_ = false;
}

void NodeView::set_obj(lv_obj_t* obj) {
  obj_ = obj;
  if (obj_) lv_obj_add_event_cb(obj_, &NodeView::obj_event_cb, LV_EVENT_DELETE, this);
}

void NodeView::release_obj(bool delete_it) {
  if (!obj_) return;
  lv_obj_t* obj = obj_;
  obj_ = nullptr;
  lv_obj_remove_event_cb_with_user_data(obj, &NodeView::obj_event_cb, this);
  if (delete_it) lv_obj_delete(obj);
}

void NodeView::swap_obj(lv_obj_t* replacement) {
  if (replacement == obj_) return;
  lv_obj_t* old = obj_;
  int32_t index = old ? lv_obj_get_index(old) : -1;
  release_obj(true);
  set_obj(replacement);
  if (replacement && index >= 0 && lv_obj_get_parent(replacement) == host_) lv_obj_move_to_index(replacement, index);
}

void NodeView::teardown(bool objects_alive) {
  props_sub_.unsubscribe();
  // Children first: they delete their own objects (or let go of them).
  auto slots = std::move(slots_);
  slots_.clear();
  for (auto& s : slots) {
    for (auto& child : s.owned) {
      if (!child) continue;
      if (!objects_alive) child->detach_objects();
      child.reset();
    }
  }
  if (instance_) {
    auto instance = std::move(instance_);
    instance->unmount(*this);
    instance.reset();
  }
  instance_type_.clear();
  instance_impl_ = nullptr;
  release_obj(objects_alive);
  fallback_ = false;
  if (node_ && node_->user_data() == this) node_->set_user_data(nullptr);
}

void NodeView::detach_objects() { teardown(false); }

void NodeView::obj_event_cb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
  auto* view = static_cast<NodeView*>(lv_event_get_user_data(e));
  // Deleted from outside (or with an ancestor): its descendants are still
  // alive but going away; let go of everything without deleting anything.
  view->obj_ = nullptr;
  view->teardown(false);
}

void NodeView::set_node(NodePtr node) {
  if (!node || node == node_) return;
  props_sub_.unsubscribe();
  if (node_ && node_->user_data() == this) node_->set_user_data(nullptr);
  const bool keep = instance_ && node->state() == NodeState::Resolved && node->type() == instance_type_ &&
                    surface_->implementation_for(*node) == instance_impl_;
  if (keep) {
    node_ = std::move(node);
    node_->set_user_data(this);
    props_sub_ = node_->on_props_changed([this](const a2ui::NodeProps& p) { on_props(p); });
    instance_->update(*this);
    return;
  }
  teardown(true);
  node_ = std::move(node);
  if (host_) render();
}

void NodeView::on_props(const a2ui::NodeProps&) {
  if (!instance_ || rendering_) return;
  instance_->update(*this);
  const Services& services = surface_->services();
  if (services.announce) {
    Accessibility a = accessibility();
    if ((a.live == "polite" || a.live == "assertive") && !a.hidden && a.label != last_announced_) {
      last_announced_ = a.label;
      services.announce(a.label, a.live == "assertive");
    }
  }
}

size_t NodeView::subscription_count() const {
  size_t n = props_sub_.active() ? 1 : 0;
  for (const auto& s : slots_) {
    for (const NodeView* v : s.views) n += v->subscription_count();
  }
  return n;
}

// --- Children --------------------------------------------------------------------

const std::vector<NodeView*>& NodeView::sync_slot(size_t slot, lv_obj_t* container,
                                                  const std::vector<ComponentNode*>& nodes, uint32_t base_index) {
  if (slots_.size() <= slot) slots_.resize(slot + 1);
  if (slots_[slot].container != container) {
    clear_slot(slot);
    slots_[slot].container = container;
  }
  // Index the current children by instance id.
  std::vector<std::unique_ptr<NodeView>> old = std::move(slots_[slot].owned);
  slots_[slot].owned.clear();
  std::unordered_map<std::string, size_t> by_id;
  if (old.size() > 8) {
    by_id.reserve(old.size());
    for (size_t i = 0; i < old.size(); ++i) by_id.emplace(old[i]->instance_id(), i);
  }
  std::vector<std::unique_ptr<NodeView>> next;
  next.reserve(nodes.size());
  for (ComponentNode* n : nodes) {
    if (!n) continue;
    std::unique_ptr<NodeView> view;
    if (old.size() > 8) {
      auto it = by_id.find(n->instance_id());
      if (it != by_id.end() && old[it->second]) view = std::move(old[it->second]);
    } else {
      for (auto& o : old) {
        if (o && o->instance_id() == n->instance_id()) {
          view = std::move(o);
          break;
        }
      }
    }
    if (view) {
      view->set_node(n->shared_from_this());
    } else {
      view = std::make_unique<NodeView>(*surface_, this, n->shared_from_this());
      view->attach(container);
    }
    next.push_back(std::move(view));
  }
  old.clear();  // Views no longer referenced: unmount and delete their objects.
  // `this` may have been re-entered above only through child construction,
  // which never touches the parent's slots, so the slot is still ours.
  Slot& s = slots_[slot];
  s.owned = std::move(next);
  s.views.clear();
  s.views.reserve(s.owned.size());
  for (auto& v : s.owned) s.views.push_back(v.get());
  // Put the objects in order (children that render nothing take no index).
  uint32_t k = base_index;
  for (NodeView* v : s.views) {
    lv_obj_t* o = v->obj();
    if (!o || lv_obj_get_parent(o) != container) continue;
    if (lv_obj_get_child(container, static_cast<int32_t>(k)) != o) lv_obj_move_to_index(o, static_cast<int32_t>(k));
    ++k;
  }
  return s.views;
}

void NodeView::clear_slot(size_t slot) {
  if (slot >= slots_.size()) return;
  auto owned = std::move(slots_[slot].owned);
  slots_[slot].owned.clear();
  slots_[slot].views.clear();
  owned.clear();
}

const std::vector<NodeView*>& NodeView::slot_views(size_t slot) const {
  return slot < slots_.size() ? slots_[slot].views : empty_views();
}

std::vector<NodeView*> NodeView::children() const {
  std::vector<NodeView*> out;
  for (const auto& s : slots_) out.insert(out.end(), s.views.begin(), s.views.end());
  return out;
}

// --- Accessibility and automation ------------------------------------------------

Accessibility NodeView::accessibility() const {
  Accessibility a;
  if (instance_) instance_->infer_accessibility(*this, &a);
  // Explicit attributes win. v1.0 surfaces carry them in `@accessibility`
  // (the core maps a legacy bare `accessibility` there); a pre-1.0 surface
  // in `accessibility`.
  const PropValue* p = prop("@accessibility");
  if (!p && node_->impl() && !node_->impl()->declares_custom("accessibility")) {
    const a2ui::SurfaceModel* model = surface_->model();
    if (model && !model->is_v1()) p = prop("accessibility");
  }
  if (p) {
    const Json attrs = p->to_json();
    if (attrs.is_object()) {
      if (attrs["label"].is_string() && !attrs["label"].as_string().empty()) a.label = attrs["label"].as_string();
      if (attrs["description"].is_string()) a.description = attrs["description"].as_string();
      if (attrs["live"].is_string()) a.live = attrs["live"].as_string();
      if (attrs["hidden"].is_bool()) a.hidden = attrs["hidden"].as_bool();
    }
  }
  if (!a.hidden) {
    for (const NodeView* v = parent_; v; v = v->parent_) {
      const PropValue* pp = v->prop("@accessibility");
      if (pp && pp->to_json()["hidden"].as_bool()) {
        a.hidden = true;
        break;
      }
    }
  }
  return a;
}

Json NodeView::describe() const {
  Json out = Json::object();
  if (instance_) instance_->describe(*this, &out);
  return out;
}

bool NodeView::simulate_input(const Json& value) { return instance_ && instance_->simulate_input(*this, value); }

bool NodeView::simulate_gesture() { return instance_ && instance_->simulate_gesture(*this); }

// --- Lookup from objects -----------------------------------------------------------

NodeView* view_of(lv_obj_t* obj) {
  if (!obj) return nullptr;
  const uint32_t n = lv_obj_get_event_count(obj);
  for (uint32_t i = 0; i < n; ++i) {
    lv_event_dsc_t* dsc = lv_obj_get_event_dsc(obj, i);
    if (dsc && lv_event_dsc_get_cb(dsc) == &NodeView::obj_event_cb) {
      return static_cast<NodeView*>(lv_event_dsc_get_user_data(dsc));
    }
  }
  return nullptr;
}

NodeView* enclosing_view(lv_obj_t* obj) {
  for (lv_obj_t* o = obj; o; o = lv_obj_get_parent(o)) {
    if (NodeView* v = view_of(o)) return v;
  }
  return nullptr;
}

std::optional<Accessibility> accessibility_of(lv_obj_t* obj) {
  NodeView* v = enclosing_view(obj);
  if (!v) return std::nullopt;
  return v->accessibility();
}

}  // namespace a2ui_lvgl

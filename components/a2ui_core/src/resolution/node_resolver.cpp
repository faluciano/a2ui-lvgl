// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// NodeResolver: the live ComponentNode tree of one surface. Ported from the
// Swift core's Resolution/NodeResolver.swift with the identity rules of the
// Python core's resolution/node_resolver.py (edge keys, occurrences,
// placeholders). See a2ui/node_resolver.h for the reactivity model.

#include "a2ui/node_resolver.h"

#include "a2ui/agent_call.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "a2ui/coerce.h"
#include "a2ui/component_model.h"
#include "a2ui/data_model.h"
#include "a2ui/surface_model.h"

namespace a2ui {

std::optional<long> scope_index_of(const std::string& data_path) {
  const auto segments = path::split(data_path);
  if (segments.empty()) return std::nullopt;
  const long i = path::parse_index(segments.back());
  if (i < 0) return std::nullopt;
  return i;
}

namespace {

constexpr const char* kPlaceholderType = "Placeholder";

std::string instance_id_for(const std::string& component_id, const std::string& data_path, int occurrence) {
  std::string id = data_path == "/" ? component_id : component_id + "-[" + data_path + "]";
  if (occurrence > 1) id += "#" + std::to_string(occurrence);
  return id;
}

std::string child_path(const std::string& base, size_t index) {
  return (base == "/" ? std::string() : base) + "/" + std::to_string(index);
}

Json coerce_dynamic(PropertyKind kind, Json v) {
  if (kind == PropertyKind::DynamicBoolean) return Json(coerce_bool(v));
  if (v.is_undefined()) return Json(nullptr);
  return v;
}

bool is_dynamic(PropertyKind kind) {
  switch (kind) {
    case PropertyKind::DynamicString:
    case PropertyKind::DynamicNumber:
    case PropertyKind::DynamicBoolean:
    case PropertyKind::DynamicStringList:
    case PropertyKind::DynamicValue:
      return true;
    default:
      return false;
  }
}

// Sorted, without duplicates and without paths inside another listed path
// (a path observer already fires for changes below it).
void minimize_paths(std::vector<std::string>* paths) {
  for (auto& p : *paths) p = path::normalize(p);
  std::sort(paths->begin(), paths->end());
  paths->erase(std::unique(paths->begin(), paths->end()), paths->end());
  std::vector<std::string> out;
  for (const auto& p : *paths) {
    bool covered = false;
    for (const auto& q : out) {
      if (path::is_same_or_descendant(p, q)) {
        covered = true;
        break;
      }
    }
    if (!covered) out.push_back(p);
  }
  *paths = std::move(out);
}

}  // namespace

namespace detail {

struct ComponentWatch {
  ComponentModel* model = nullptr;
  Subscription updated;
  Subscription metadata;
  std::vector<ComponentNode*> nodes;
};

struct ResolverState;

/**
 * Resolves the properties of one component definition in a data scope.
 * With a ResolverState it builds node props (child nodes, bindings);
 * without one it builds a snapshot (child references left as written).
 */
struct Binder {
  explicit Binder(DataContext& c) : ctx(c) {}

  DataContext& ctx;
  ResolverState* rs = nullptr;
  ComponentNode* owner = nullptr;
  std::vector<std::pair<std::string, NodePtr>>* edges = nullptr;
  std::vector<std::string>* deps = nullptr;
  bool snapshot = false;
  std::map<std::string, int> occurrences;

  NodeProps bind_all(const ComponentModel& model);
  PropValue property(const std::string& slot, const Json& raw, PropertyKind kind,
                     const std::map<std::string, PropertyKind>* nested);
  PropValue dynamic(const Json& raw, PropertyKind kind);
  PropValue literal(const Json& raw) {
    Json v = ctx.resolve_value(raw);
    return PropValue::literal(v.is_undefined() ? Json(nullptr) : std::move(v));
  }
  PropValue child(const std::string& slot, const std::string& component_id, const std::string& data_path,
                  std::optional<long> index);
  PropValue child_list(const std::string& slot, const Json& raw);
  PropValue nested_object(const std::string& slot, const Json& raw,
                          const std::map<std::string, PropertyKind>& kinds);
  PropValue nested_value(const std::string& slot, const Json& raw,
                         const std::map<std::string, PropertyKind>& kinds);
};

struct ResolverState : std::enable_shared_from_this<ResolverState> {
  SurfaceModel* surface = nullptr;
  NodeResolverOptions options;
  Signal<NodePtr> root;
  EventSource<ComponentNode*> children_changed;
  std::map<std::string, ComponentWatch, std::less<>> watches;
  std::vector<NodePtr> dirty;
  std::vector<NodePtr> children_changed_queue;
  std::set<std::string> dispatched;
  SubscriptionBag surface_subs;
  bool root_dirty = false;
  bool flushing = false;
  bool flush_queued = false;
  bool disposed = false;
  uint64_t next_id = 0;
  size_t live_nodes = 0;
  size_t dep_subs = 0;
  size_t watch_subs = 0;

  ~ResolverState() { dispose(); }

  void attach(SurfaceModel& s, NodeResolverOptions opts) {
    surface = &s;
    options = opts;
    if (options.max_depth < 1) options.max_depth = 1;
    surface_subs.add(s.components_model().on_created().subscribe(
        [this](ComponentModel* const& m) { on_component_created(m); }));
    surface_subs.add(s.components_model().on_deleted().subscribe(
        [this](const std::string& id) { on_component_deleted(id); }));
    surface_subs.add(s.data_model().on_changed().subscribe([this](const std::string&) { request_flush(); }));
    surface_subs.add(s.on_disposed().subscribe([this] { dispose(); }));
    surface_subs.add(s.on_agent_call_settled().subscribe(
        [this](const AgentCallSettlement& settled) { on_agent_call_settled(settled.key); }));
    root_dirty = true;
    flush();
  }

  // --- Scheduling -------------------------------------------------------------

  /** Re-resolves the nodes whose latest pass made the agent call that settled. */
  void on_agent_call_settled(const std::string& key) {
    if (disposed) return;
    for (auto& w : watches) {
      for (ComponentNode* n : w.second.nodes) {
        if (n->agent_tracker_ && n->agent_tracker_->makes_call(key)) mark_dirty(n);
      }
    }
    request_flush();
  }

  void mark_dirty(ComponentNode* n) {
    if (n->disposed_ || n->dirty_) return;
    n->dirty_ = true;
    dirty.push_back(n->shared_from_this());
  }

  void schedule_parent(ComponentNode* n) {
    if (n->parent_) {
      mark_dirty(n->parent_);
    } else {
      root_dirty = true;
    }
  }

  void request_flush() {
    if (disposed || flushing) return;
    auto& batch = a2ui::detail::batch_state();
    if (batch.depth > 0) {
      if (flush_queued) return;
      flush_queued = true;
      std::weak_ptr<ResolverState> weak = shared_from_this();
      batch.pending.push_back([weak] {
        if (auto s = weak.lock()) {
          s->flush_queued = false;
          s->flush();
        }
      });
      return;
    }
    flush();
  }

  void flush() {
    if (disposed || flushing) return;
    if (!root_dirty && dirty.empty()) return;
    auto keep = shared_from_this();
    flushing = true;
    {
      Batch batch;  // Props of every node change in this pass notify together, at the end.
      while (!disposed) {
        if (root_dirty) {
          root_dirty = false;
          sync_root();
          continue;
        }
        if (dirty.empty()) break;
        // Parents first: a parent's pass may replace or dispose a dirty child.
        size_t best = 0;
        for (size_t i = 1; i < dirty.size(); ++i) {
          if (dirty[i]->depth_ < dirty[best]->depth_) best = i;
        }
        NodePtr n = std::move(dirty[best]);
        dirty.erase(dirty.begin() + static_cast<std::ptrdiff_t>(best));
        n->dirty_ = false;
        if (!n->disposed_) resolve(n.get());
      }
      flushing = false;
      auto changed = std::move(children_changed_queue);
      children_changed_queue.clear();
      for (const auto& n : changed) {
        if (!n->disposed_ && !disposed) children_changed.emit(n.get());
      }
    }
  }

  // --- Surface events -------------------------------------------------------------

  void on_component_created(ComponentModel* m) {
    if (disposed) return;
    auto it = watches.find(m->id());
    if (it != watches.end()) {
      ComponentWatch& w = it->second;
      detach_watch(&w);
      attach_watch(&w, m);
      for (ComponentNode* n : w.nodes) schedule_parent(n);
    }
    if (m->id() == surface->root_id()) root_dirty = true;
    request_flush();
  }

  void on_component_deleted(const std::string& id) {
    if (disposed) return;
    auto it = watches.find(id);
    if (it != watches.end()) {
      ComponentWatch& w = it->second;
      detach_watch(&w);
      for (ComponentNode* n : w.nodes) {
        n->model_ = nullptr;
        schedule_parent(n);
      }
    }
    if (id == surface->root_id()) root_dirty = true;
    request_flush();
  }

  void on_definition_updated(ComponentWatch* w) {
    for (ComponentNode* n : w->nodes) {
      if (n->state_ == NodeState::Resolved) mark_dirty(n);
    }
    request_flush();  // Last: the flush may drop `w`.
  }

  void on_definition_metadata(ComponentWatch* w, const Json& metadata) {
    const std::vector<ComponentNode*> nodes = w->nodes;
    for (ComponentNode* n : nodes) {
      if (n->state_ == NodeState::Resolved || n->state_ == NodeState::UnknownType) n->metadata_.set(metadata);
    }
  }

  // --- Definition observers, one per component id ------------------------------------

  void attach_watch(ComponentWatch* w, ComponentModel* m) {
    w->model = m;
    w->updated = m->on_updated().subscribe([this, w](const ComponentModel* const&) { on_definition_updated(w); });
    w->metadata = m->on_metadata_changed().subscribe([this, w](const Json& md) { on_definition_metadata(w, md); });
    watch_subs += 2;
  }

  void detach_watch(ComponentWatch* w) {
    if (!w->model) return;
    w->updated.unsubscribe();
    w->metadata.unsubscribe();
    w->model = nullptr;
    watch_subs -= 2;
  }

  void register_node(ComponentNode* n) {
    auto it = watches.find(n->component_id_);
    if (it == watches.end()) it = watches.emplace(n->component_id_, ComponentWatch()).first;
    ComponentWatch& w = it->second;
    if (!w.model) {
      if (ComponentModel* m = surface->components_model().get(n->component_id_)) attach_watch(&w, m);
    }
    w.nodes.push_back(n);
  }

  void unregister_node(ComponentNode* n) {
    auto it = watches.find(n->component_id_);
    if (it == watches.end()) return;
    auto& nodes = it->second.nodes;
    nodes.erase(std::remove(nodes.begin(), nodes.end(), n), nodes.end());
    if (nodes.empty()) {
      detach_watch(&it->second);
      watches.erase(it);
    }
  }

  // --- Node lifecycle -------------------------------------------------------------

  static bool up_to_date(const ComponentNode& n, const ComponentModel* model) {
    switch (n.state_) {
      case NodeState::Pending:
        return model == nullptr;
      case NodeState::UnknownType:
        return model && !model->api() && n.model_ == model;
      case NodeState::Resolved:
        return model && model->api() && n.model_ == model && n.type_ == model->type();
      case NodeState::Cyclic:
        return false;
    }
    return false;
  }

  static bool is_cyclic(const ComponentNode* parent, const std::string& component_id, const std::string& data_path) {
    for (const ComponentNode* a = parent; a; a = a->parent_) {
      if (a->component_id_ == component_id && a->data_path_ == data_path) return true;
    }
    return false;
  }

  void dispatch_once(const char* code, const std::string& component_id, const std::string& data_path,
                     std::string message) {
    std::string key = std::string(code) + "|" + component_id + "|" + data_path;
    if (!dispatched.insert(std::move(key)).second) return;
    surface->dispatch_error(Json::object({{"code", Json(code)}, {"message", Json(std::move(message))}}));
  }

  NodePtr create_node(ComponentNode* parent, const std::string& component_id,
                      const std::string& data_path, std::optional<long> index, int occurrence,
                      ComponentModel* model, bool cyclic) {
    NodePtr n(new ComponentNode());
    n->id_ = "a2ui-node-" + std::to_string(++next_id);
    n->instance_id_ = instance_id_for(component_id, data_path, occurrence);
    n->component_id_ = component_id;
    n->data_path_ = data_path;
    n->index_ = index;
    n->depth_ = parent ? parent->depth_ + 1 : 0;
    n->parent_ = parent;
    n->surface_ = surface;
    if (cyclic) {
      n->state_ = NodeState::Cyclic;
      n->type_ = model ? model->type() : kPlaceholderType;
      if (n->depth_ > options.max_depth) {
        dispatch_once("CYCLIC_REFERENCE", component_id, data_path,
                      "Component '" + component_id + "' at '" + data_path + "' exceeds the maximum depth of " +
                          std::to_string(options.max_depth) + "; omitting the reference.");
      } else {
        dispatch_once("CYCLIC_REFERENCE", component_id, data_path,
                      "Component '" + component_id + "' at '" + data_path +
                          "' is referenced by one of its own descendants; omitting the reference.");
      }
    } else if (!model) {
      n->state_ = NodeState::Pending;
      n->type_ = kPlaceholderType;
    } else if (!model->api()) {
      n->state_ = NodeState::UnknownType;
      n->type_ = model->type();
      n->model_ = model;
      n->metadata_.reset(model->metadata());
      dispatch_once("UNKNOWN_COMPONENT_TYPE", component_id, data_path,
                    "Component '" + component_id + "' has type '" + model->type() +
                        "', which is not in catalog '" + model->catalog().id() + "'.");
    } else {
      n->state_ = NodeState::Resolved;
      n->type_ = model->type();
      n->model_ = model;
      n->impl_ = model->api();
      n->metadata_.reset(model->metadata());
    }
    register_node(n.get());
    ++live_nodes;
    if (n->state_ == NodeState::Resolved) {
      mark_dirty(n.get());  // Resolved in this pass, after its parent.
    } else {
      n->initialized_ = true;
    }
    return n;
  }

  NodePtr child_node(ComponentNode* parent, const std::string& edge_key, const std::string& component_id,
                     const std::string& data_path, std::optional<long> index, int occurrence) {
    NodePtr existing;
    for (const auto& e : parent->edges_) {
      if (e.first == edge_key) {
        existing = e.second;
        break;
      }
    }
    ComponentModel* model = surface->components_model().get(component_id);
    if (parent->depth_ + 1 > options.max_depth || is_cyclic(parent, component_id, data_path)) {
      if (existing && existing->state_ == NodeState::Cyclic) return existing;
      return create_node(parent, component_id, data_path, index, occurrence, model, true);
    }
    if (existing && !existing->disposed_ && up_to_date(*existing, model)) return existing;
    return create_node(parent, component_id, data_path, index, occurrence, model, false);
  }

  void sync_root() {
    const std::string& root_id = surface->root_id();
    ComponentModel* m = surface->components_model().get(root_id);
    NodePtr cur = root.value();
    if (cur && m && up_to_date(*cur, m)) return;
    if (cur) dispose_tree(cur);
    NodePtr next;
    if (m) next = create_node(nullptr, root_id, "/", std::nullopt, 1, m, false);
    root.set(std::move(next));
  }

  void resolve(ComponentNode* n) {
    if (n->state_ != NodeState::Resolved || !n->model_) {
      n->initialized_ = true;
      return;
    }
    std::vector<std::string> deps;
    std::vector<std::pair<std::string, NodePtr>> edges;
    if (!n->agent_tracker_) n->agent_tracker_ = std::make_shared<AgentCallTracker>(*surface);
    n->agent_tracker_->begin_pass();
    DataContext ctx(surface->data_model(), n->agent_tracker_.get(), n->data_path_, n->index_);
    ctx.set_dependency_sink(&deps);
    Binder b(ctx);
    b.rs = this;
    b.owner = n;
    b.edges = &edges;
    b.deps = &deps;
    NodeProps props = b.bind_all(*n->model_);
    if (n->disposed_ || disposed) return;  // A function reached back and tore this node down.

    // Children that are no longer referenced (or were replaced) go away.
    for (const auto& old : n->edges_) {
      bool kept = false;
      for (const auto& e : edges) kept = kept || e.second == old.second;
      if (!kept) dispose_tree(old.second);
    }
    bool children_changed = false;
    if (n->initialized_) {
      children_changed = n->edges_.size() != edges.size();
      for (size_t i = 0; !children_changed && i < edges.size(); ++i) {
        children_changed = n->edges_[i].second != edges[i].second;
      }
    }
    n->edges_ = std::move(edges);
    update_deps(n, std::move(deps));
    if (!n->initialized_) {
      n->props_.reset(std::move(props));
      n->initialized_ = true;
    } else {
      n->props_.set(std::move(props));
    }
    if (children_changed) children_changed_queue.push_back(n->shared_from_this());
  }

  void update_deps(ComponentNode* n, std::vector<std::string> deps) {
    minimize_paths(&deps);
    if (deps == n->deps_ && n->dep_subs_.size() == deps.size()) return;
    dep_subs -= n->dep_subs_.size();
    n->dep_subs_.clear();
    n->deps_ = std::move(deps);
    n->dep_subs_.reserve(n->deps_.size());
    for (const auto& p : n->deps_) {
      n->dep_subs_.push_back(surface->data_model().subscribe(p, [this, n](const Json&) { mark_dirty(n); }));
    }
    dep_subs += n->dep_subs_.size();
  }

  void dispose_tree(const NodePtr& top) {
    std::vector<NodePtr> stack{top};
    while (!stack.empty()) {
      NodePtr n = std::move(stack.back());
      stack.pop_back();
      if (n->disposed_) continue;
      for (const auto& e : n->edges_) stack.push_back(e.second);
      dispose_one(n.get());
    }
  }

  void dispose_one(ComponentNode* n) {
    n->disposed_ = true;
    dep_subs -= n->dep_subs_.size();
    n->dep_subs_.clear();
    n->deps_.clear();
    n->agent_tracker_.reset();
    unregister_node(n);
    if (n->state_ == NodeState::UnknownType) {
      dispatched.erase("UNKNOWN_COMPONENT_TYPE|" + n->component_id_ + "|" + n->data_path_);
    } else if (n->state_ == NodeState::Cyclic) {
      dispatched.erase("CYCLIC_REFERENCE|" + n->component_id_ + "|" + n->data_path_);
    }
    n->edges_.clear();
    n->parent_ = nullptr;
    n->surface_ = nullptr;
    n->model_ = nullptr;
    --live_nodes;
    auto cleanups = std::move(n->cleanups_);
    n->cleanups_.clear();
    for (auto& fn : cleanups) fn();
    n->destroyed_.emit();
    n->destroyed_.clear();
    n->props_.clear_listeners();
    n->metadata_.clear_listeners();
    n->props_.reset(NodeProps());  // Releases the subtree this node held.
  }

  void dispose() {
    if (disposed) return;
    disposed = true;
    surface_subs.clear();
    if (NodePtr r = root.value()) {
      dispose_tree(r);
      root.set(nullptr);
    }
    for (auto& w : watches) detach_watch(&w.second);
    watches.clear();
    dirty.clear();
    children_changed_queue.clear();
    children_changed.clear();
    surface = nullptr;
  }
};

// ---------------------------------------------------------------------------
// Binder

NodeProps Binder::bind_all(const ComponentModel& model) {
  const ComponentApi* api = model.api();
  std::vector<PropMember> members;
  const Json* rules = nullptr;
  for (const auto& m : model.properties().as_object()) {
    PropertyKind kind = api ? api->kind_of(m.first) : PropertyKind::Static;
    if (m.first == "@checks") kind = PropertyKind::Checks;
    if (kind == PropertyKind::Checks) {
      // The rules pass on as written; their outcome is published below.
      if (!rules) rules = &m.second;
      members.push_back({m.first, PropValue::literal(m.second)});
      continue;
    }
    members.push_back({m.first, property(m.first, m.second, kind, api ? api->nested_kinds_of(m.first) : nullptr)});
  }
  if (rules) {
    Json outcome = evaluate_checks(*rules, ctx);
    for (const char* field : {"isValid", "validationErrors", "validationResults"}) {
      members.push_back({field, PropValue::literal(outcome[field])});
    }
    if (snapshot) members.push_back({"validationResult", PropValue::literal(outcome["validationResult"])});
  }
  return NodeProps(std::move(members));
}

PropValue Binder::property(const std::string& slot, const Json& raw, PropertyKind kind,
                           const std::map<std::string, PropertyKind>* nested) {
  if (is_dynamic(kind)) return dynamic(raw, kind);
  switch (kind) {
    case PropertyKind::ComponentId:
      if (rs && raw.is_string() && !raw.as_string().empty()) {
        return child(slot, raw.as_string(), owner->data_path_, owner->index_);
      }
      return PropValue::literal(raw);
    case PropertyKind::ChildList:
      if (!rs) return PropValue::literal(raw);
      return child_list(slot, raw);
    case PropertyKind::Action:
      return PropValue::action(raw);
    case PropertyKind::Checks:
      return PropValue::literal(raw);
    default:
      break;
  }
  if (nested) return nested_value(slot, raw, *nested);
  return literal(raw);
}

PropValue Binder::dynamic(const Json& raw, PropertyKind kind) {
  std::string bound;
  if (const Json* p = raw.find("@path"); p && p->is_string()) bound = ctx.absolute_path(p->as_string());
  return PropValue::binding(coerce_dynamic(kind, ctx.resolve_value(raw)), std::move(bound));
}

PropValue Binder::child(const std::string& slot, const std::string& component_id, const std::string& data_path,
                        std::optional<long> index) {
  std::string occurrence_key = component_id + "@" + data_path;
  const int occurrence = ++occurrences[occurrence_key];
  std::string edge = slot + ">" + occurrence_key + "#" + std::to_string(occurrence);
  NodePtr n = rs->child_node(owner, edge, component_id, data_path, index, occurrence);
  edges->emplace_back(std::move(edge), n);
  return PropValue::node(std::move(n));
}

PropValue Binder::child_list(const std::string& slot, const Json& raw) {
  if (raw.is_array()) {
    std::vector<PropValue> items;
    items.reserve(raw.size());
    for (const auto& item : raw.as_array()) {
      if (item.is_string() && !item.as_string().empty()) {
        items.push_back(child(slot, item.as_string(), owner->data_path_, owner->index_));
      } else if (item.is_object() && item["componentId"].is_string() && !item["componentId"].as_string().empty()) {
        items.push_back(child(slot, item["componentId"].as_string(), owner->data_path_, owner->index_));
      } else {
        items.push_back(PropValue::literal(item));
      }
    }
    return PropValue::array(std::move(items));
  }
  if (raw.is_object() && raw["componentId"].is_string()) {
    const Json& raw_path = raw["path"].is_string() ? raw["path"] : raw["@path"];
    if (raw_path.is_string()) {
      // A template: one child per item of the array at the path, scoped to the item.
      const std::string list_path = ctx.absolute_path(raw_path.as_string());
      if (deps) deps->push_back(list_path);
      const Json list = ctx.model().value_at(list_path);
      std::vector<PropValue> items;
      const std::string& component_id = raw["componentId"].as_string();
      if (list.is_array() && !component_id.empty()) {
        items.reserve(list.size());
        for (size_t i = 0; i < list.size(); ++i) {
          items.push_back(child(slot, component_id, child_path(list_path, i), static_cast<long>(i)));
        }
      }
      return PropValue::array(std::move(items));
    }
  }
  return PropValue::literal(raw);
}

PropValue Binder::nested_object(const std::string& slot, const Json& raw,
                                const std::map<std::string, PropertyKind>& kinds) {
  std::vector<PropMember> members;
  members.reserve(raw.size());
  for (const auto& m : raw.as_object()) {
    auto it = kinds.find(m.first);
    const PropertyKind kind = it == kinds.end() ? PropertyKind::Static : it->second;
    members.push_back({m.first, property(slot + "." + m.first, m.second, kind, nullptr)});
  }
  return PropValue::object(std::move(members));
}

PropValue Binder::nested_value(const std::string& slot, const Json& raw,
                               const std::map<std::string, PropertyKind>& kinds) {
  if (raw.is_object() && !raw.contains("@path") && !raw.contains("@call")) return nested_object(slot, raw, kinds);
  if (raw.is_array()) {
    std::vector<PropValue> items;
    items.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
      const Json& item = raw[i];
      if (item.is_object() && !item.contains("@path") && !item.contains("@call")) {
        items.push_back(nested_object(slot + "[" + std::to_string(i) + "]", item, kinds));
      } else {
        items.push_back(literal(item));
      }
    }
    return PropValue::array(std::move(items));
  }
  return literal(raw);
}

}  // namespace detail

// ---------------------------------------------------------------------------
// NodeResolver

NodeResolver::NodeResolver(SurfaceModel& surface, NodeResolverOptions options)
    : state_(std::make_shared<detail::ResolverState>()) {
  state_->attach(surface, options);
}

NodeResolver::~NodeResolver() { dispose(); }

const Signal<NodePtr>& NodeResolver::root_node() const { return state_->root; }
ComponentNode* NodeResolver::root() const { return state_->root.value().get(); }
const EventSource<ComponentNode*>& NodeResolver::on_children_changed() const { return state_->children_changed; }
SurfaceModel* NodeResolver::surface() const { return state_->surface; }
bool NodeResolver::disposed() const { return state_->disposed; }
void NodeResolver::dispose() { state_->dispose(); }
size_t NodeResolver::node_count() const { return state_->live_nodes; }

size_t NodeResolver::active_subscription_count() const {
  return state_->dep_subs + state_->watch_subs + state_->surface_subs.size();
}

// ---------------------------------------------------------------------------
// One-shot resolution

Json resolve_component_snapshot(SurfaceModel& surface, const ComponentModel& component, const std::string& data_path) {
  DataContext ctx(surface.data_model(), &surface, data_path, scope_index_of(data_path));
  detail::Binder b(ctx);
  b.snapshot = true;
  return b.bind_all(component).to_json();
}

}  // namespace a2ui

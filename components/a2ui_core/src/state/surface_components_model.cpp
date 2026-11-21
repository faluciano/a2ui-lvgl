// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// SurfaceComponentsModel and the component graph checks: composition
// constraints, root presence, dangling references, cycles and depth, and
// reachability (Python state/validation_helpers.py, Swift
// GraphTopologyValidator), plus ComponentGraphBound, which lets a batch be
// checked against what it changes rather than the whole surface.

#include <algorithm>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "a2ui/component_model.h"
#include "a2ui/schema.h"

namespace a2ui {
namespace {

std::string python_list(const std::vector<std::string>& v) {
  std::string out = "[";
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += ", ";
    out += "'" + v[i] + "'";
  }
  return out + "]";
}

bool contains_str(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

Error integrity(std::string msg) { return make_error(ErrorKind::Integrity, std::move(msg)); }
Error recursion(std::string msg) { return make_error(ErrorKind::Recursion, std::move(msg)); }

// A reference's field as the other SDKs print it: "children[0]", "tabs[1].child".
std::string field_of(const std::string& pointer) {
  std::string out;
  size_t i = pointer.empty() ? 0 : 1;
  while (i <= pointer.size() && !pointer.empty()) {
    size_t j = pointer.find('/', i);
    if (j == std::string::npos) j = pointer.size();
    const std::string seg = pointer.substr(i, j - i);
    const bool numeric = !seg.empty() && seg.find_first_not_of("0123456789") == std::string::npos;
    if (numeric) {
      out += "[" + seg + "]";
    } else {
      if (!out.empty()) out += '.';
      out += seg;
    }
    i = j + 1;
  }
  return out;
}

// The held components overlaid with a batch.
struct Candidate {
  const SurfaceComponentsModel::Map& held;
  std::unordered_map<std::string_view, const ComponentModel*> batch;
  std::unordered_map<std::string_view, size_t> batch_index;

  const ComponentModel* get(std::string_view id) const {
    if (auto it = batch.find(id); it != batch.end()) return it->second;
    auto it = held.find(id);
    return it == held.end() ? nullptr : it->second.get();
  }
  bool has(std::string_view id) const { return get(id) != nullptr; }

  // Every component, in the order the other SDKs iterate the merged map:
  // held components in the order they were added (a replaced one keeps its
  // place), then the batch's new ones in batch order.
  std::vector<const ComponentModel*> ordered(const std::vector<std::unique_ptr<ComponentModel>>& b) const {
    std::vector<const ComponentModel*> held_list;
    held_list.reserve(held.size());
    for (const auto& m : held) held_list.push_back(m.second.get());
    std::sort(held_list.begin(), held_list.end(),
              [](const ComponentModel* x, const ComponentModel* y) { return seq(x) < seq(y); });
    std::vector<const ComponentModel*> out;
    out.reserve(held_list.size() + b.size());
    for (const ComponentModel* c : held_list) out.push_back(get(c->id()));
    for (const auto& c : b) {
      if (held.find(c->id()) == held.end()) out.push_back(c.get());
    }
    return out;
  }
  static uint64_t seq(const ComponentModel* c) { return c->insertion_order(); }
};

std::string component_pointer(const Candidate& g, const std::string& id) {
  if (auto it = g.batch_index.find(id); it != g.batch_index.end()) {
    return "/components/" + std::to_string(it->second);
  }
  return "/components/" + escape_pointer_token(id);
}

Error composition_error(const char* code, std::string msg, std::string pointer, const std::string& surface_id) {
  Error e = make_error(ErrorKind::Validation, msg, code);
  e.path = pointer;
  e.surface_id = surface_id;
  Json detail = Json::object({{"path", Json(pointer)}, {"code", Json(code)}, {"message", Json(std::move(msg))}});
  if (!surface_id.empty()) detail.set("surfaceId", Json(surface_id));
  e.details = Json::array({std::move(detail)});
  return e;
}

Status check_allowed_parents(const Candidate& g, const ComponentModel& c,
                             const std::vector<std::pair<std::string, std::string>>& parents, bool is_root,
                             const std::string& surface_id) {
  const ComponentApi* api = c.api();
  if (!api || !api->allowed_parents || api->allowed_parents->empty()) return ok_status();
  const auto& allowed = *api->allowed_parents;
  auto check = [&](const std::string& pid, const std::string& ptype) -> Status {
    if (contains_str(allowed, ptype)) return ok_status();
    return composition_error("UNALLOWED_PARENT",
                             "Component '" + c.id() + "' (" + c.type() + ") cannot be placed under parent '" +
                                 pid + "' (" + ptype + "). Allowed parents: " + python_list(allowed) + ".",
                             component_pointer(g, c.id()), surface_id);
  };
  if (is_root) A2UI_RETURN_IF_ERROR(check("Surface", "Surface"));
  for (const auto& p : parents) A2UI_RETURN_IF_ERROR(check(p.first, p.second));
  return ok_status();
}

Status check_allowed_children(const Candidate& g, const ComponentModel& c, const std::vector<ChildRef>& refs,
                              const std::string& surface_id) {
  const ComponentApi* api = c.api();
  if (!api || !api->allowed_children || api->allowed_children->empty()) return ok_status();
  const auto& allowed = *api->allowed_children;
  for (const auto& r : refs) {
    const ComponentModel* child = g.get(r.id);
    if (!child || contains_str(allowed, child->type())) continue;
    return composition_error("UNALLOWED_CHILD",
                             "Container '" + c.id() + "' (" + c.type() + ") cannot contain child '" + r.id +
                                 "' (" + child->type() + "). Allowed children: " + python_list(allowed) + ".",
                             component_pointer(g, c.id()) + r.path, surface_id);
  }
  return ok_status();
}

bool declares_composition(const ComponentModel& c) {
  const ComponentApi* api = c.api();
  return api && ((api->allowed_parents && !api->allowed_parents->empty()) ||
                 (api->allowed_children && !api->allowed_children->empty()));
}

bool catalog_declares_composition(const Catalog& cat) {
  for (const auto& c : cat.components()) {
    if ((c.allowed_parents && !c.allowed_parents->empty()) ||
        (c.allowed_children && !c.allowed_children->empty())) {
      return true;
    }
  }
  return false;
}

using Adjacency = std::unordered_map<std::string_view, std::vector<ChildRef>>;

Adjacency build_adjacency(const std::vector<const ComponentModel*>& all) {
  Adjacency adj;
  adj.reserve(all.size());
  for (const ComponentModel* c : all) c->child_references(&adj[c->id()]);
  return adj;
}

Status validate_composition(const Candidate& g, const std::vector<const ComponentModel*>& all,
                            const Adjacency& adj, const std::string& root_id, const std::string& surface_id) {
  std::unordered_map<std::string_view, std::vector<std::pair<std::string, std::string>>> parents;
  for (const ComponentModel* c : all) {
    auto it = adj.find(c->id());
    if (it == adj.end()) continue;
    for (const auto& r : it->second) parents[r.id].emplace_back(c->id(), c->type());
  }
  static const std::vector<std::pair<std::string, std::string>> kNone;
  for (const ComponentModel* c : all) {
    if (!declares_composition(*c)) continue;
    auto pit = parents.find(c->id());
    A2UI_RETURN_IF_ERROR(check_allowed_parents(g, *c, pit == parents.end() ? kNone : pit->second,
                                               c->id() == root_id, surface_id));
    auto ait = adj.find(c->id());
    static const std::vector<ChildRef> kNoRefs;
    A2UI_RETURN_IF_ERROR(check_allowed_children(g, *c, ait == adj.end() ? kNoRefs : ait->second, surface_id));
  }
  return ok_status();
}

Status validate_integrity(const Candidate& g, const std::vector<const ComponentModel*>& all, const Adjacency& adj,
                          const std::string& root_id, const ValidationConfig& config) {
  if (!config.allow_missing_root && !g.has(root_id)) {
    return integrity("Missing root component: No component has id='" + root_id + "'");
  }
  if (config.allow_dangling_references) return ok_status();
  for (const ComponentModel* c : all) {
    for (const auto& r : adj.at(c->id())) {
      if (!g.has(r.id)) {
        return integrity("Dangling reference: Component '" + c->id() + "' references non-existent component '" +
                         r.id + "' in field '" + field_of(r.path) + "'");
      }
    }
  }
  return ok_status();
}

// analyze_topology: self-references, cycles and depth, then reachability.
struct Topology {
  const Candidate& g;
  const Adjacency& adj;
  int max_depth;
  bool allow_cycles;
  std::unordered_set<std::string_view> visited, on_stack;

  Status dfs(std::string_view id, int depth) {
    if (depth > max_depth) {
      return recursion("Global recursion limit exceeded: logical depth > " + std::to_string(max_depth));
    }
    visited.insert(id);
    on_stack.insert(id);
    auto it = adj.find(id);
    if (it != adj.end()) {
      for (const auto& r : it->second) {
        if (!g.has(r.id)) continue;
        const std::string_view nid = g.get(r.id)->id();
        if (!visited.count(nid)) {
          A2UI_RETURN_IF_ERROR(dfs(nid, depth + 1));
        } else if (on_stack.count(nid) && !allow_cycles) {
          return recursion("Circular reference detected involving component '" + r.id + "'");
        }
      }
    }
    on_stack.erase(id);
    return ok_status();
  }
};

Result<std::set<std::string>> analyze_topology(const Candidate& g, const std::vector<const ComponentModel*>& all,
                                               const Adjacency& adj, const std::string& root_id,
                                               const ValidationConfig& config, bool check_orphans) {
  for (const ComponentModel* c : all) {
    for (const auto& r : adj.at(c->id())) {
      if (r.id == c->id()) {
        return Error(recursion("Self-reference detected: Component '" + c->id() + "' references itself in field '" +
                               field_of(r.path) + "'"));
      }
    }
  }
  Topology t{g, adj, config.max_depth.value_or(kMaxGlobalDepth), config.allow_cyclic_references, {}, {}};
  if (config.allow_missing_root) {
    std::vector<std::string_view> ids;
    for (const ComponentModel* c : all) ids.push_back(c->id());
    std::sort(ids.begin(), ids.end());
    for (std::string_view id : ids) {
      if (!t.visited.count(id)) {
        Status s = t.dfs(id, 0);
        if (!s.ok()) return s.take_error();
      }
    }
  } else {
    if (const ComponentModel* root = g.get(root_id)) {
      Status s = t.dfs(root->id(), 0);
      if (!s.ok()) return s.take_error();
    }
    if (check_orphans && !config.allow_orphan_components) {
      std::vector<std::string_view> orphans;
      for (const ComponentModel* c : all) {
        if (!t.visited.count(c->id())) orphans.push_back(c->id());
      }
      if (!orphans.empty()) {
        std::sort(orphans.begin(), orphans.end());
        return Error(integrity("Component '" + std::string(orphans.front()) + "' is not reachable from '" +
                               root_id + "'"));
      }
    }
  }
  std::set<std::string> out;
  for (std::string_view v : t.visited) out.emplace(v);
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// ComponentGraphBound (Python state/component_graph_bound.py)

class ComponentGraphBound {
 public:
  static std::unique_ptr<ComponentGraphBound> of(const std::vector<const ComponentModel*>& all) {
    auto b = std::make_unique<ComponentGraphBound>();
    std::vector<ChildRef> refs;
    for (const ComponentModel* c : all) {
      refs.clear();
      c->child_references(&refs);
      b->set_children(c->id(), refs);
    }
    b->compute_heights(all);
    return b;
  }

  bool composition_holds(const Candidate& g, const SurfaceComponentsModel::Batch& batch,
                         const std::string& root_id) const {
    std::unordered_map<std::string_view, std::vector<ChildRef>> declared;
    std::unordered_map<std::string, std::set<std::string>> batch_parents;
    for (const auto& c : batch) {
      auto& refs = declared[c->id()];
      c->child_references(&refs);
      for (const auto& r : refs) batch_parents[r.id].insert(c->id());
    }
    auto parents_of = [&](const std::string& id) {
      std::set<std::string> ps;
      if (auto it = parents_.find(id); it != parents_.end()) {
        for (const auto& p : it->second) {
          if (!g.batch.count(p)) ps.insert(p);
        }
      }
      if (auto it = batch_parents.find(id); it != batch_parents.end()) ps.insert(it->second.begin(), it->second.end());
      std::vector<std::pair<std::string, std::string>> out;
      for (const auto& p : ps) {
        const ComponentModel* m = g.get(p);
        out.emplace_back(p, m ? m->type() : std::string("Unknown"));
      }
      return out;
    };
    std::set<std::string> moved;
    std::map<std::string, std::vector<std::string>> holding;
    for (const auto& c : batch) {
      if (auto it = children_.find(c->id()); it != children_.end()) moved.insert(it->second.begin(), it->second.end());
      for (const auto& r : declared[c->id()]) moved.insert(r.id);
      if (auto it = parents_.find(c->id()); it != parents_.end()) {
        for (const auto& p : it->second) {
          if (!g.batch.count(p)) holding[p].push_back(c->id());
        }
      }
    }
    auto check = [&](const std::string& id) -> bool {
      const ComponentModel* m = g.get(id);
      if (!m || !declares_composition(*m)) return true;
      const bool in_batch = g.batch.count(id) > 0;
      if (in_batch || moved.count(id)) {
        if (!check_allowed_parents(g, *m, parents_of(id), id == root_id, "").ok()) return false;
      }
      if (in_batch) {
        if (!check_allowed_children(g, *m, declared[m->id()], "").ok()) return false;
      } else if (auto it = holding.find(id); it != holding.end()) {
        std::vector<ChildRef> refs;
        for (const auto& cid : it->second) refs.push_back({cid, ""});
        if (!check_allowed_children(g, *m, refs, "").ok()) return false;
      }
      return true;
    };
    for (const auto& c : batch) {
      if (!check(c->id())) return false;
    }
    for (const auto& id : moved) {
      if (!check(id)) return false;
    }
    for (const auto& h : holding) {
      if (!check(h.first)) return false;
    }
    return true;
  }

  /** Whether the graph with the batch provably has no self-reference, cycle or chain deeper than `limit`. */
  bool admits(const Candidate& g, const SurfaceComponentsModel::Batch& batch, int limit) {
    if (!heights_valid_ || max_height_ > limit) return false;
    std::vector<ChildRef> refs;
    for (const auto& c : batch) {
      refs.clear();
      c->child_references(&refs);
      for (const auto& r : refs) {
        if (r.id == c->id()) return false;
      }
      set_children(c->id(), refs);
    }
    std::deque<std::string> rising;
    for (const auto& c : batch) {
      int h = 0;
      for (const auto& child : children_[c->id()]) {
        auto it = heights_.find(child);
        const int below = it == heights_.end() ? 0 : it->second;
        if (g.has(child) && below >= h) h = below + 1;
      }
      heights_[c->id()] = h;
      rising.push_back(c->id());
    }
    while (!rising.empty()) {
      const std::string id = std::move(rising.front());
      rising.pop_front();
      const int h = heights_[id];
      if (h > limit) return false;
      max_height_ = std::max(max_height_, h);
      auto pit = parents_.find(id);
      if (pit == parents_.end()) continue;
      for (const auto& p : pit->second) {
        int& ph = heights_[p];
        if (ph <= h) {
          ph = h + 1;
          rising.push_back(p);
        }
      }
    }
    return true;
  }

 /** Records the batch's edges only; the depth bound no longer holds. */
  void update_edges(const SurfaceComponentsModel::Batch& batch) {
    std::vector<ChildRef> refs;
    for (const auto& c : batch) {
      refs.clear();
      c->child_references(&refs);
      set_children(c->id(), refs);
    }
    heights_valid_ = false;
  }

 private:
  void set_children(const std::string& id, const std::vector<ChildRef>& refs) {
    auto& kids = children_[id];
    for (const auto& k : kids) {
      auto it = parents_.find(k);
      if (it == parents_.end()) continue;
      it->second.erase(id);
      if (it->second.empty()) parents_.erase(it);
    }
    kids.clear();
    for (const auto& r : refs) {
      kids.push_back(r.id);
      parents_[r.id].insert(id);
    }
  }

  // Heights children first, iteratively (no recursion on device stacks).
  void compute_heights(const std::vector<const ComponentModel*>& all) {
    for (const ComponentModel* start : all) {
      if (heights_.count(start->id())) continue;
      struct Frame {
        std::string id;
        size_t next;
      };
      std::vector<Frame> stack{{start->id(), 0}};
      std::unordered_set<std::string> on_stack{start->id()};
      while (!stack.empty()) {
        Frame& f = stack.back();
        const auto& kids = children_[f.id];
        bool pushed = false;
        while (f.next < kids.size()) {
          const std::string& child = kids[f.next++];
          if (!children_.count(child) || heights_.count(child)) continue;
          if (on_stack.count(child)) {
            max_height_ = 1 << 30;  // A cycle: no bound holds.
            return;
          }
          on_stack.insert(child);
          stack.push_back({child, 0});
          pushed = true;
          break;
        }
        if (pushed) continue;
        int h = 0;
        for (const auto& child : children_[f.id]) {
          if (!children_.count(child)) continue;
          h = std::max(h, heights_[child] + 1);
        }
        heights_[f.id] = h;
        max_height_ = std::max(max_height_, h);
        on_stack.erase(f.id);
        stack.pop_back();
      }
    }
  }

  std::unordered_map<std::string, std::vector<std::string>> children_;
  std::unordered_map<std::string, std::set<std::string>> parents_;
  std::unordered_map<std::string, int> heights_;
  int max_height_ = 0;
  bool heights_valid_ = true;
};

// ---------------------------------------------------------------------------
// SurfaceComponentsModel

SurfaceComponentsModel::SurfaceComponentsModel() = default;
SurfaceComponentsModel::~SurfaceComponentsModel() {
  for (auto& m : components_) m.second->dispose();
}

ComponentModel* SurfaceComponentsModel::get(std::string_view id) const {
  auto it = components_.find(id);
  return it == components_.end() ? nullptr : it->second.get();
}

void SurfaceComponentsModel::adopt(ComponentModel* c) {
  c->owner_revision_ = &revision_;
  c->seq_ = next_seq_++;
}

Status SurfaceComponentsModel::add_component(std::unique_ptr<ComponentModel> component) {
  if (!component) return make_error(ErrorKind::State, "Cannot add a null component.");
  if (has(component->id())) {
    return make_error(ErrorKind::State, "Component with id '" + component->id() + "' already exists.");
  }
  ComponentModel* raw = component.get();
  adopt(raw);
  components_.emplace(raw->id(), std::move(component));
  note_change();
  on_created_.emit(raw);
  return ok_status();
}

void SurfaceComponentsModel::remove_component(const std::string& id) {
  auto it = components_.find(id);
  if (it == components_.end()) return;
  std::unique_ptr<ComponentModel> old = std::move(it->second);
  components_.erase(it);
  note_change();
  old->dispose();
  on_deleted_.emit(id);
}

void SurfaceComponentsModel::replace_component(std::unique_ptr<ComponentModel> component) {
  if (!component) return;
  ComponentModel* raw = component.get();
  auto it = components_.find(raw->id());
  std::unique_ptr<ComponentModel> old;
  if (it != components_.end()) {
    old = std::move(it->second);
    adopt(raw);
    raw->seq_ = old->seq_;  // A replaced component keeps its place.
    it->second = std::move(component);
  } else {
    adopt(raw);
    components_.emplace(raw->id(), std::move(component));
  }
  note_change();
  if (old) {
    old->dispose();
    on_deleted_.emit(raw->id());
  }
  on_created_.emit(raw);
}

std::vector<std::string> SurfaceComponentsModel::get_child_ids(std::string_view id) const {
  std::vector<std::string> out;
  const ComponentModel* c = get(id);
  if (!c) return out;
  std::vector<ChildRef> refs;
  c->child_references(&refs);
  out.reserve(refs.size());
  for (auto& r : refs) out.push_back(std::move(r.id));
  return out;
}

Status SurfaceComponentsModel::validate_components_update(const Batch& batch,
                                                          const std::optional<ValidationConfig>& config,
                                                          const GraphCheckOptions& options) {
  Candidate g{components_, {}, {}};
  g.batch.reserve(batch.size());
  for (size_t i = 0; i < batch.size(); ++i) {
    const std::string& id = batch[i]->id();
    if (g.batch.count(id)) {
      const std::string msg = "Duplicate component ID '" + id + "' in update payload.";
      Error e = integrity(msg);
      e.path = "/components/" + std::to_string(i) + "/id";
      e.surface_id = options.surface_id;
      e.details = Json::array({Json::object({{"path", Json("/components/" + escape_pointer_token(id))},
                                             {"code", Json("duplicate_id")},
                                             {"message", Json(msg)}})});
      return e;
    }
    g.batch.emplace(id, batch[i].get());
    g.batch_index.emplace(id, i);
  }

  const bool bound_valid = bound_ && !bound_pending_ && bound_revision_ == revision_;
  std::unique_ptr<ComponentGraphBound> bound = bound_valid ? std::move(bound_) : nullptr;
  bound_.reset();
  bound_pending_ = false;

  auto tag = [&](Status s) -> Status {
    if (s.ok()) return s;
    Error e = s.take_error();
    if (e.surface_id.empty()) e.surface_id = options.surface_id;
    return e;
  };

  // Without a config only composition constraints are checked, and only
  // when a catalog in play declares any.
  const bool deferred = !config || (config->allow_dangling_references &&
                                    (config->allow_orphan_components || config->allow_missing_root));
  if (!config) {
    bool any_rules = false;
    for (const auto& c : batch) any_rules = any_rules || catalog_declares_composition(c->catalog());
    if (!any_rules) {
      std::set<const Catalog*> seen;
      for (const auto& m : components_) {
        const Catalog* cat = &m.second->catalog();
        if (seen.insert(cat).second && catalog_declares_composition(*cat)) {
          any_rules = true;
          break;
        }
      }
    }
    if (!any_rules) {
      // The bound (if any) stays valid for edges only when it is rebuilt;
      // drop it and let a later check rebuild it.
      return ok_status();
    }
  }

  if (deferred && bound) {
    if (!bound->composition_holds(g, batch, options.root_id)) {
      auto all = g.ordered(batch);
      Adjacency adj = build_adjacency(all);
      A2UI_RETURN_IF_ERROR(tag(validate_composition(g, all, adj, options.root_id, options.surface_id)));
    }
    if (config && !config->allow_missing_root && !g.has(options.root_id)) {
      return tag(integrity("Missing root component: No component has id='" + options.root_id + "'"));
    }
    const int limit = config && config->max_depth ? *config->max_depth : kMaxGlobalDepth;
    if (!config) bound->update_edges(batch);  // Composition only: keep the edges current.
    if (!config || bound->admits(g, batch, limit)) {
      bound_ = std::move(bound);
      bound_pending_ = true;
      return ok_status();
    }
    auto all = g.ordered(batch);
    Adjacency adj = build_adjacency(all);
    auto topo = analyze_topology(g, all, adj, options.root_id, *config, true);
    if (!topo.ok()) return tag(topo.take_error());
    bound_ = ComponentGraphBound::of(all);
    bound_pending_ = true;
    return ok_status();
  }

  auto all = g.ordered(batch);
  Adjacency adj = build_adjacency(all);
  A2UI_RETURN_IF_ERROR(tag(validate_composition(g, all, adj, options.root_id, options.surface_id)));
  if (config) {
    A2UI_RETURN_IF_ERROR(tag(validate_integrity(g, all, adj, options.root_id, *config)));
    auto topo = analyze_topology(g, all, adj, options.root_id, *config, true);
    if (!topo.ok()) return tag(topo.take_error());
  }
  if (deferred) {
    bound_ = ComponentGraphBound::of(all);
    bound_pending_ = true;
  }
  return ok_status();
}

void SurfaceComponentsModel::batch_applied() {
  if (bound_ && bound_pending_) {
    bound_pending_ = false;
    bound_revision_ = revision_;
  }
}

Result<std::set<std::string>> SurfaceComponentsModel::detect_cycles(const ValidationConfig& config,
                                                                     const std::string& root_id) const {
  Candidate g{components_, {}, {}};
  auto all = g.ordered({});
  Adjacency adj = build_adjacency(all);
  ValidationConfig cfg = config;
  cfg.allow_orphan_components = true;
  return analyze_topology(g, all, adj, root_id, cfg, false);
}

Status SurfaceComponentsModel::validate_topology(const ValidationConfig& config, const std::string& root_id) const {
  if (components_.empty()) return ok_status();
  Candidate g{components_, {}, {}};
  auto all = g.ordered({});
  Adjacency adj = build_adjacency(all);
  A2UI_RETURN_IF_ERROR(validate_integrity(g, all, adj, root_id, config));
  auto topo = analyze_topology(g, all, adj, root_id, config, true);
  if (!topo.ok()) return topo.take_error();
  return ok_status();
}

std::vector<Error> SurfaceComponentsModel::validate_references(const ValidationConfig& config,
                                                               const std::string& root_id) const {
  Status s = validate_topology(config, root_id);
  if (s.ok()) return {};
  return {s.error()};
}

void SurfaceComponentsModel::dispose() {
  std::vector<std::string> ids;
  ids.reserve(components_.size());
  for (const auto& m : components_) ids.push_back(m.first);
  for (const auto& id : ids) remove_component(id);
  bound_.reset();
  on_created_.clear();
  on_deleted_.clear();
}

}  // namespace a2ui

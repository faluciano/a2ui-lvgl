// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// NodeResolver (blueprint section F and features/node_resolution): keeps a
// live tree of ComponentNodes for one SurfaceModel, so that an adapter reads
// ready-to-use values instead of resolving definitions itself.
//
// Reactivity, without per-update tree rebuilds:
//   - Each node records the data paths its resolution read (DataContext
//     dependency sink) and observes exactly those; a data write marks the
//     affected nodes dirty and only those re-resolve.
//   - Definitions are observed once per component id, shared by every node
//     built from that id: a property update re-resolves those nodes, an
//     envelope metadata update sets their metadata only, and a creation or
//     deletion re-resolves their parents (placeholder <-> concrete node).
//   - Dirty nodes are re-resolved in one pass ("flush"), parents before
//     children, iteratively (no recursion proportional to tree depth). The
//     flush runs when the write that caused it completes, or at the end of
//     the enclosing Batch (MessageProcessor applies each message in one).
//   - A node emits props only when its resolved value changed. Identity is
//     preserved: a child keeps its node object while its component id, data
//     scope, definition and type stay the same; an explicit list reorder
//     keeps each child's node; a template item is keyed by its index.
//
// Lifetime: the resolver must not outlive the SurfaceModel unless the
// surface is disposed first (deleteSurface); surface disposal disposes the
// tree. Single-threaded like the rest of the core.

#pragma once

#include <memory>
#include <string>

#include "a2ui/component_node.h"
#include "a2ui/data_context.h"
#include "a2ui/events.h"
#include "a2ui/json.h"
#include "a2ui/ref.h"

namespace a2ui {

class SurfaceModel;
class ComponentModel;

struct NodeResolverOptions {
  /** Deepest node level; a reference below it resolves to a cyclic placeholder. */
  int max_depth = 50;
};

class NodeResolver {
 public:
  explicit NodeResolver(SurfaceModel& surface, NodeResolverOptions options = {});
  ~NodeResolver();
  NodeResolver(const NodeResolver&) = delete;
  NodeResolver& operator=(const NodeResolver&) = delete;

  /** The resolved root; null until the surface holds its root component. */
  const Signal<NodePtr>& root_node() const;
  ComponentNode* root() const;

  /**
   * Fires after an update pass for each surviving node whose list of child
   * nodes changed (a child added, removed, replaced or reordered), parents
   * first, before the pass's props notifications are delivered. Lets an
   * adapter re-sync child views with one listener for the whole tree.
   */
  const EventSource<ComponentNode*>& on_children_changed() const;

  /** The surface; null once disposed. */
  SurfaceModel* surface() const;
  bool disposed() const;

  /** Disposes every node (each fires on_destroyed once) and stops tracking the surface. */
  void dispose();

  /** Live (mounted, not disposed) nodes. */
  size_t node_count() const;
  /**
   * Subscriptions the resolver holds on the surface: data path observers,
   * definition observers and its own surface listeners. 0 after dispose().
   */
  size_t active_subscription_count() const;

 private:
  Ref<detail::ResolverState> state_;
};

// ---------------------------------------------------------------------------
// One-shot resolution (no tree, no subscriptions)

/**
 * Evaluates check rules (CheckRule list) in `ctx` and normalizes each
 * outcome to a ValidationResult. Returns
 * {isValid, validationErrors, validationResults, validationResult}:
 * isValid is false iff a failed result has severity "error";
 * validationErrors are those messages, validationResults every failed
 * result, both in rule order; validationResult is the first failure (or
 * {valid: true}).
 */
Json evaluate_checks(const Json& rules, DataContext& ctx);

/**
 * Resolves one component definition at a data scope into plain JSON, as a
 * component binder snapshot: dynamic values resolved (DynamicBoolean
 * coerced), child references and actions left as written, checks evaluated
 * into isValid / validationErrors / validationResults / validationResult.
 * @index inside the scope is its last segment when that is an array index.
 */
Json resolve_component_snapshot(SurfaceModel& surface, const ComponentModel& component,
                                const std::string& data_path = "/");

/** The template item index a data scope ends in ("/items/3" -> 3), if any. */
std::optional<long> scope_index_of(const std::string& data_path);

}  // namespace a2ui

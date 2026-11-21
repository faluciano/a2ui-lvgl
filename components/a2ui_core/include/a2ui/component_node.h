// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// ComponentNode: one resolved component instance at one position of a
// surface's tree (blueprints/features/node_resolution.blueprint.md, ported
// from the Swift core's Models/Node.swift and Models/Resolved*.swift).
//
// A node's `props` hold one PropValue per property of the component
// definition:
//   - Binding  for a Dynamic* property: the current value, plus the bound
//              absolute data path when the payload bound one (writable);
//   - Action   for an Action property: the raw action payload, resolved
//              and dispatched only when invoked (ComponentNode::invoke);
//   - Node     for a child reference (ComponentId, or an item of a
//              ChildList, explicit or template);
//   - Array / Object  for containers holding any of the above;
//   - Literal  for everything else (nested bindings already resolved).
// Checkable components also get `isValid`, `validationErrors` and
// `validationResults` literals.
//
// Props are snapshots: a change arrives as a new NodeProps through
// props_signal(), never by mutating one already delivered.
//
// Memory: nodes are shared_ptr-owned by their parent's props (the root by
// the resolver). A node kept alive by an adapter after its position went
// away is disposed(): inert, with no subscriptions.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "a2ui/catalog.h"
#include "a2ui/error.h"
#include "a2ui/events.h"
#include "a2ui/json.h"

namespace a2ui {

class AgentCallTracker;

class ComponentModel;
class ComponentNode;
class SurfaceModel;
using NodePtr = std::shared_ptr<ComponentNode>;

namespace detail {
struct ResolverState;
struct Binder;
}  // namespace detail

/** Whether a node is resolved or stands in for a component that cannot be resolved there. */
enum class NodeState : uint8_t { Resolved, Pending, UnknownType, Cyclic };

/** "resolved", "pending", "unknown-type" or "cyclic". */
const char* node_state_name(NodeState state);

struct PropMember;

/** One resolved property value (see the file comment). Immutable once built. */
class PropValue {
 public:
  enum class Kind : uint8_t { Literal, Binding, Action, Node, Array, Object };

  PropValue() = default;
  static PropValue literal(Json value);
  /** `path` is the absolute bound path, or empty for a read-only binding. */
  static PropValue binding(Json value, std::string path = {});
  static PropValue action(Json payload);
  static PropValue node(NodePtr node);
  static PropValue array(std::vector<PropValue> items);
  static PropValue object(std::vector<PropMember> members);

  Kind kind() const { return kind_; }
  bool is_literal() const { return kind_ == Kind::Literal; }
  bool is_binding() const { return kind_ == Kind::Binding; }
  bool is_action() const { return kind_ == Kind::Action; }
  bool is_node() const { return kind_ == Kind::Node; }
  bool is_array() const { return kind_ == Kind::Array; }
  bool is_object() const { return kind_ == Kind::Object; }

  /** Literal: the value. Binding: the current value. Action: the raw payload. Else undefined. */
  const Json& value() const { return value_; }
  /** A binding whose payload bound a data path. */
  bool writable() const { return kind_ == Kind::Binding && !path_.empty(); }
  /** The absolute bound path of a writable binding; empty otherwise. */
  const std::string& path() const { return path_; }
  /** The child node of a Node value; null otherwise. */
  ComponentNode* node() const { return node_.get(); }
  const NodePtr& node_ptr() const { return node_; }
  /** Array items (empty unless is_array()). */
  const std::vector<PropValue>& items() const;
  /** Object members in definition order (empty unless is_object()). */
  const std::vector<PropMember>& members() const;
  /** Object member by key, or null. */
  const PropValue* find(std::string_view key) const;

  /**
   * A plain JSON view: bindings as their value, actions as their payload,
   * nodes as their component id.
   */
  Json to_json() const;

  /** Deep equality; nodes compare by identity. */
  friend bool operator==(const PropValue& a, const PropValue& b);
  friend bool operator!=(const PropValue& a, const PropValue& b) { return !(a == b); }

 private:
  Kind kind_ = Kind::Literal;
  Json value_;
  std::string path_;
  NodePtr node_;
  std::shared_ptr<const std::vector<PropValue>> items_;
  std::shared_ptr<const std::vector<PropMember>> members_;
};

struct PropMember {
  std::string key;
  PropValue value;
  friend bool operator==(const PropMember& a, const PropMember& b) { return a.key == b.key && a.value == b.value; }
};

/** A node's resolved properties, keyed by the component's schema property names, in definition order. */
class NodeProps {
 public:
  NodeProps() = default;
  explicit NodeProps(std::vector<PropMember> members) : members_(std::move(members)) {}

  const PropValue* find(std::string_view key) const;
  bool contains(std::string_view key) const { return find(key) != nullptr; }
  const std::vector<PropMember>& members() const { return members_; }
  size_t size() const { return members_.size(); }
  bool empty() const { return members_.empty(); }
  std::vector<PropMember>::const_iterator begin() const { return members_.begin(); }
  std::vector<PropMember>::const_iterator end() const { return members_.end(); }

  /** Plain JSON view (see PropValue::to_json). */
  Json to_json() const;

  friend bool operator==(const NodeProps& a, const NodeProps& b) { return a.members_ == b.members_; }
  friend bool operator!=(const NodeProps& a, const NodeProps& b) { return !(a == b); }

 private:
  std::vector<PropMember> members_;
};

class ComponentNode : public std::enable_shared_from_this<ComponentNode> {
 public:
  ComponentNode(const ComponentNode&) = delete;
  ComponentNode& operator=(const ComponentNode&) = delete;
  ~ComponentNode();

  /** Names the component at this node's data scope; distinct among siblings. Key sibling views by it. */
  const std::string& instance_id() const { return instance_id_; }
  /** Unique within the resolver for the life of this node object; a replacement gets a new one. */
  const std::string& id() const { return id_; }
  /** The component id from the payload. */
  const std::string& component_id() const { return component_id_; }
  /** The declared component type; "Placeholder" for a pending node. */
  const std::string& type() const { return type_; }
  /** The data scope relative paths resolve against ("/", "/items/0"). */
  const std::string& data_path() const { return data_path_; }
  /** The template item index this node's scope is in (for @index), if any. */
  std::optional<long> index() const { return index_; }
  NodeState state() const { return state_; }
  bool is_placeholder() const { return state_ != NodeState::Resolved; }
  bool disposed() const { return disposed_; }
  /** The catalog entry of a resolved node; null for placeholders. */
  const ComponentApi* impl() const { return impl_; }
  /** The node whose props hold this one; null for the root and once disposed. */
  ComponentNode* parent() const { return parent_; }

  /** Current resolved properties. */
  const NodeProps& props() const { return props_.value(); }
  /** Emits when, and only when, this node's resolved properties change (once per update). */
  const Signal<NodeProps>& props_signal() const { return props_; }
  [[nodiscard]] Subscription on_props_changed(std::function<void(const NodeProps&)> fn) const {
    return props_.subscribe(std::move(fn));
  }
  /** Shorthand for props().find(key). */
  const PropValue* prop(std::string_view key) const { return props().find(key); }

  /** The definition's envelope `@metadata`; undefined for pending and cyclic nodes. */
  const Json& metadata() const { return metadata_.value(); }
  /** Emits when, and only when, the envelope metadata changes. */
  const Signal<Json>& metadata_signal() const { return metadata_; }

  /** Fires once, when the resolver disposes this node. */
  const EventSource<>& on_destroyed() const { return destroyed_; }
  /** Runs `cleanup` when this node is disposed (immediately if it already is). */
  void add_cleanup(std::function<void()> cleanup);

  /** Check results (true / empty when the component carries no checks). */
  bool is_valid() const;
  Json validation_errors() const;
  Json validation_results() const;

  /** Appends the child nodes held in props, in property order (depth-first through containers). */
  void children(std::vector<ComponentNode*>* out) const;

  /**
   * Writes through a writable binding of this node (top-level property
   * `property`, or any binding found in its props). ErrorKind::State if
   * the node is disposed or the property is not a writable binding.
   */
  Status write(std::string_view property, Json value);
  Status write(const PropValue& binding, Json value);

  /**
   * Invokes an action of this node: resolves the payload now, in this
   * node's data scope, with user activation. An event is dispatched as a
   * client action (with optional `metadata`, an action.metadata envelope);
   * a function call runs and emits no action. Failures are reported on the
   * surface's error channel; the returned status says whether it ran.
   */
  Status invoke(std::string_view property, const Json& metadata = Json());
  Status invoke(const PropValue& action, const Json& metadata = Json());

  /** An opaque slot for the adapter (e.g. the lv_obj_t* rendering this node). */
  void* user_data() const { return user_data_; }
  void set_user_data(void* p) { user_data_ = p; }

 private:
  friend struct detail::ResolverState;
  friend struct detail::Binder;
  ComponentNode() = default;

  std::string instance_id_;
  std::string id_;
  std::string component_id_;
  std::string type_;
  std::string data_path_;
  std::optional<long> index_;
  NodeState state_ = NodeState::Pending;
  bool disposed_ = false;
  bool dirty_ = false;
  bool initialized_ = false;
  int depth_ = 0;
  const ComponentApi* impl_ = nullptr;
  ComponentModel* model_ = nullptr;  // The definition this node was built from; null once deleted.
  ComponentNode* parent_ = nullptr;
  SurfaceModel* surface_ = nullptr;
  Signal<NodeProps> props_;
  Signal<Json> metadata_;
  EventSource<> destroyed_;
  std::vector<std::function<void()>> cleanups_;
  std::vector<std::string> deps_;
  std::vector<Subscription> dep_subs_;
  std::vector<std::pair<std::string, NodePtr>> edges_;  // Child nodes by edge key.
  // Evaluates this node's functions and remembers which agent calls it made,
  // so a settled call re-resolves exactly the nodes that read it.
  std::shared_ptr<AgentCallTracker> agent_tracker_;
  void* user_data_ = nullptr;
};

}  // namespace a2ui

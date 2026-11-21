// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// PropValue, NodeProps and the ComponentNode operations that do not need
// the resolver (writes, actions, accessors). See a2ui/component_node.h.

#include "a2ui/component_node.h"

#include <utility>

#include "a2ui/surface_model.h"

namespace a2ui {

const char* node_state_name(NodeState state) {
  switch (state) {
    case NodeState::Resolved: return "resolved";
    case NodeState::Pending: return "pending";
    case NodeState::UnknownType: return "unknown-type";
    case NodeState::Cyclic: return "cyclic";
  }
  return "resolved";
}

// ---------------------------------------------------------------------------
// PropValue

namespace {
const std::vector<PropValue>& empty_items() {
  static const std::vector<PropValue> kEmpty;
  return kEmpty;
}
const std::vector<PropMember>& empty_members() {
  static const std::vector<PropMember> kEmpty;
  return kEmpty;
}
}  // namespace

PropValue PropValue::literal(Json value) {
  PropValue p;
  p.kind_ = Kind::Literal;
  p.value_ = std::move(value);
  return p;
}

PropValue PropValue::binding(Json value, std::string path) {
  PropValue p;
  p.kind_ = Kind::Binding;
  p.value_ = std::move(value);
  p.path_ = std::move(path);
  return p;
}

PropValue PropValue::action(Json payload) {
  PropValue p;
  p.kind_ = Kind::Action;
  p.value_ = std::move(payload);
  return p;
}

PropValue PropValue::node(NodePtr node) {
  PropValue p;
  p.kind_ = Kind::Node;
  p.node_ = std::move(node);
  return p;
}

PropValue PropValue::array(std::vector<PropValue> items) {
  PropValue p;
  p.kind_ = Kind::Array;
  p.items_ = std::make_shared<const std::vector<PropValue>>(std::move(items));
  return p;
}

PropValue PropValue::object(std::vector<PropMember> members) {
  PropValue p;
  p.kind_ = Kind::Object;
  p.members_ = std::make_shared<const std::vector<PropMember>>(std::move(members));
  return p;
}

const std::vector<PropValue>& PropValue::items() const { return items_ ? *items_ : empty_items(); }

const std::vector<PropMember>& PropValue::members() const { return members_ ? *members_ : empty_members(); }

const PropValue* PropValue::find(std::string_view key) const {
  for (const auto& m : members()) {
    if (m.key == key) return &m.value;
  }
  return nullptr;
}

Json PropValue::to_json() const {
  switch (kind_) {
    case Kind::Literal:
    case Kind::Binding:
    case Kind::Action:
      return value_;
    case Kind::Node:
      return node_ ? Json(node_->component_id()) : Json(nullptr);
    case Kind::Array: {
      Json out{Json::Array{}};
      for (const auto& item : items()) out.push_back(item.to_json());
      return out;
    }
    case Kind::Object: {
      Json out = Json::object();
      for (const auto& m : members()) out.set(m.key, m.value.to_json());
      return out;
    }
  }
  return Json();
}

bool operator==(const PropValue& a, const PropValue& b) {
  if (a.kind_ != b.kind_) return false;
  switch (a.kind_) {
    case PropValue::Kind::Literal:
    case PropValue::Kind::Action:
      return a.value_ == b.value_;
    case PropValue::Kind::Binding:
      return a.path_ == b.path_ && a.value_ == b.value_;
    case PropValue::Kind::Node:
      return a.node_ == b.node_;
    case PropValue::Kind::Array:
      return a.items_ == b.items_ || a.items() == b.items();
    case PropValue::Kind::Object:
      return a.members_ == b.members_ || a.members() == b.members();
  }
  return false;
}

// ---------------------------------------------------------------------------
// NodeProps

const PropValue* NodeProps::find(std::string_view key) const {
  for (const auto& m : members_) {
    if (m.key == key) return &m.value;
  }
  return nullptr;
}

Json NodeProps::to_json() const {
  Json out = Json::object();
  for (const auto& m : members_) out.set(m.key, m.value.to_json());
  return out;
}

// ---------------------------------------------------------------------------
// ComponentNode

ComponentNode::~ComponentNode() = default;

void ComponentNode::add_cleanup(std::function<void()> cleanup) {
  if (!cleanup) return;
  if (disposed_) {
    cleanup();
    return;
  }
  cleanups_.push_back(std::move(cleanup));
}

bool ComponentNode::is_valid() const {
  const PropValue* v = prop("isValid");
  return !v || v->value().as_bool(true);
}

Json ComponentNode::validation_errors() const {
  const PropValue* v = prop("validationErrors");
  return v && v->value().is_array() ? v->value() : Json(Json::Array{});
}

Json ComponentNode::validation_results() const {
  const PropValue* v = prop("validationResults");
  return v && v->value().is_array() ? v->value() : Json(Json::Array{});
}

namespace {
void collect_children(const PropValue& v, std::vector<ComponentNode*>* out) {
  switch (v.kind()) {
    case PropValue::Kind::Node:
      if (v.node()) out->push_back(v.node());
      break;
    case PropValue::Kind::Array:
      for (const auto& item : v.items()) collect_children(item, out);
      break;
    case PropValue::Kind::Object:
      for (const auto& m : v.members()) collect_children(m.value, out);
      break;
    default:
      break;
  }
}

Error state_error(std::string message) { return make_error(ErrorKind::State, std::move(message)); }
}  // namespace

void ComponentNode::children(std::vector<ComponentNode*>* out) const {
  for (const auto& m : props()) collect_children(m.value, out);
}

Status ComponentNode::write(std::string_view property, Json value) {
  const PropValue* b = prop(property);
  if (!b) return state_error("Node '" + instance_id_ + "' has no property '" + std::string(property) + "'.");
  return write(*b, std::move(value));
}

Status ComponentNode::write(const PropValue& binding, Json value) {
  if (disposed_ || !surface_) return state_error("Node '" + instance_id_ + "' is disposed.");
  if (!binding.writable()) {
    return state_error("Property of node '" + instance_id_ + "' is not a writable binding.");
  }
  return surface_->data_model().set(binding.path(), std::move(value));
}

Status ComponentNode::invoke(std::string_view property, const Json& metadata) {
  const PropValue* a = prop(property);
  if (!a) return state_error("Node '" + instance_id_ + "' has no property '" + std::string(property) + "'.");
  return invoke(*a, metadata);
}

Status ComponentNode::invoke(const PropValue& action, const Json& metadata) {
  if (disposed_ || !surface_) return state_error("Node '" + instance_id_ + "' is disposed.");
  if (!action.is_action()) return state_error("Property of node '" + instance_id_ + "' is not an action.");
  const Json payload = action.value();  // Copy: dispatch may re-resolve this node.
  const bool is_event = payload["event"].is_object() || payload.contains("name");
  SurfaceModel* surface = surface_;
  const std::string source = component_id_;
  if (surface->routes_to_agent()) {
    // The action may need the agent: run it asynchronously so it can wait for
    // answers. Failures are reported on the surface error channel.
    surface->run_action(payload, source, data_path_, /*user_activated=*/true, nullptr, metadata);
    return ok_status();
  }
  Json resolved = surface->resolve_action(payload, data_path_, /*user_activated=*/true);
  if (!is_event) return ok_status();  // A function call ran (or reported its failure).
  if (resolved.is_null()) return make_error(ErrorKind::Validation, "Action of '" + source + "' failed to resolve.");
  return surface->dispatch_action(resolved, source, metadata);
}

}  // namespace a2ui

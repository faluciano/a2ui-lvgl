// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// SchemaNode and PackedBuffer. See a2ui/schema_node.h.

#include "a2ui/schema_node.h"

#include <cmath>
#include <map>
#include <utility>

namespace a2ui {

namespace {

constexpr uint32_t kTypeBits = 3;
constexpr uint32_t kTypeMask = (1u << kTypeBits) - 1;

uint32_t word0_of(Json::Type t, size_t count) {
  return static_cast<uint32_t>(t) | static_cast<uint32_t>(count << kTypeBits);
}

}  // namespace

SchemaNode::SchemaNode(const Json* json) : json_(json && !json->is_undefined() ? json : nullptr) {}

Json::Type SchemaNode::type() const {
  if (json_) return json_->type();
  if (doc_) return static_cast<Json::Type>(word0() & kTypeMask);
  return Json::Type::Undefined;
}

bool SchemaNode::as_bool(bool fallback) const {
  if (json_) return json_->as_bool(fallback);
  if (is_bool()) return (word0() >> kTypeBits) != 0;
  return fallback;
}

double SchemaNode::as_number(double fallback) const {
  if (json_) return json_->as_number(fallback);
  if (is_number()) return doc_->numbers[word1()];
  return fallback;
}

std::string_view SchemaNode::as_string() const {
  if (json_) return json_->as_string();
  if (is_string()) return std::string_view(doc_->strings + word1(), word0() >> kTypeBits);
  return {};
}

size_t SchemaNode::size() const {
  if (json_) return json_->size();
  if (is_array() || is_object()) return word0() >> kTypeBits;
  return 0;
}

SchemaNode SchemaNode::find(std::string_view key) const {
  if (json_) return SchemaNode(json_->find(key));
  if (!is_object()) return {};
  const uint32_t n = word0() >> kTypeBits;
  const uint32_t first = word1();
  for (uint32_t i = 0; i < n; ++i) {
    const SchemaNode k(doc_, first + 2 * i);
    if (k.as_string() == key) return SchemaNode(doc_, first + 2 * i + 1);
  }
  return {};
}

SchemaNode SchemaNode::at(size_t index) const {
  if (json_) return json_->is_array() && index < json_->size() ? SchemaNode(&(*json_)[index]) : SchemaNode();
  if (!is_array() || index >= size()) return {};
  return SchemaNode(doc_, word1() + static_cast<uint32_t>(index));
}

std::string_view SchemaNode::key_at(size_t index) const {
  if (json_) return json_->is_object() && index < json_->size() ? std::string_view(json_->as_object()[index].first)
                                                                 : std::string_view();
  if (!is_object() || index >= size()) return {};
  return SchemaNode(doc_, word1() + 2 * static_cast<uint32_t>(index)).as_string();
}

SchemaNode SchemaNode::value_at(size_t index) const {
  if (json_) return json_->is_object() && index < json_->size() ? SchemaNode(&json_->as_object()[index].second)
                                                                 : SchemaNode();
  if (!is_object() || index >= size()) return {};
  return SchemaNode(doc_, word1() + 2 * static_cast<uint32_t>(index) + 1);
}

bool SchemaNode::equals(const Json& other) const {
  if (json_) return *json_ == other;
  if (type() != other.type()) return false;
  switch (type()) {
    case Json::Type::Undefined:
    case Json::Type::Null: return true;
    case Json::Type::Bool: return as_bool() == other.as_bool();
    case Json::Type::Number: return as_number() == other.as_number();
    case Json::Type::String: return as_string() == other.as_string();
    case Json::Type::Array: {
      if (size() != other.size()) return false;
      for (size_t i = 0; i < size(); ++i) {
        if (!at(i).equals(other[i])) return false;
      }
      return true;
    }
    case Json::Type::Object: {
      if (size() != other.size()) return false;
      for (size_t i = 0; i < size(); ++i) {
        const Json* o = other.find(key_at(i));
        if (!o || !value_at(i).equals(*o)) return false;
      }
      return true;
    }
  }
  return false;
}

bool SchemaNode::equals(const SchemaNode& other) const {
  if (other.json_) return equals(*other.json_);
  if (json_) return other.equals(*json_);
  if (type() != other.type()) return false;
  switch (type()) {
    case Json::Type::Undefined:
    case Json::Type::Null: return true;
    case Json::Type::Bool: return as_bool() == other.as_bool();
    case Json::Type::Number: return as_number() == other.as_number();
    case Json::Type::String: return as_string() == other.as_string();
    case Json::Type::Array: {
      if (size() != other.size()) return false;
      for (size_t i = 0; i < size(); ++i) {
        if (!at(i).equals(other.at(i))) return false;
      }
      return true;
    }
    case Json::Type::Object: {
      if (size() != other.size()) return false;
      for (size_t i = 0; i < size(); ++i) {
        const SchemaNode o = other.find(key_at(i));
        if (!o.present() || !value_at(i).equals(o)) return false;
      }
      return true;
    }
  }
  return false;
}

Json SchemaNode::to_json() const {
  if (json_) return *json_;
  switch (type()) {
    case Json::Type::Undefined: return Json();
    case Json::Type::Null: return Json(nullptr);
    case Json::Type::Bool: return Json(as_bool());
    case Json::Type::Number: return Json(as_number());
    case Json::Type::String: return Json(as_string());
    case Json::Type::Array: {
      Json::Array a;
      a.reserve(size());
      for (size_t i = 0; i < size(); ++i) a.push_back(at(i).to_json());
      return Json(std::move(a));
    }
    case Json::Type::Object: {
      Json::Object o;
      o.reserve(size());
      for (size_t i = 0; i < size(); ++i) o.emplace_back(std::string(key_at(i)), value_at(i).to_json());
      return Json(std::move(o));
    }
  }
  return Json();
}

std::string SchemaNode::dump() const {
  if (json_) return json_->dump();
  return to_json().dump();
}

// ---------------------------------------------------------------------------
// PackedBuffer

PackedBuffer::PackedBuffer(const Json& value) {
  // Breadth first: a container's children are reserved as one block, and
  // nested containers get theirs when they come off the queue.
  std::map<std::string, uint32_t> string_offsets;
  std::map<double, uint32_t> number_index;
  auto intern = [&](std::string_view s) -> uint32_t {
    auto it = string_offsets.find(std::string(s));
    if (it != string_offsets.end()) return it->second;
    const uint32_t off = static_cast<uint32_t>(strings_.size());
    strings_.append(s);
    string_offsets.emplace(std::string(s), off);
    return off;
  };
  auto set_node = [&](uint32_t index, uint32_t w0, uint32_t w1) {
    nodes_[2 * index] = w0;
    nodes_[2 * index + 1] = w1;
  };
  std::vector<std::pair<uint32_t, const Json*>> queue;
  nodes_.assign(2, 0);
  queue.emplace_back(0, &value);
  for (size_t q = 0; q < queue.size(); ++q) {
    const uint32_t index = queue[q].first;
    const Json& v = *queue[q].second;
    const uint32_t next = static_cast<uint32_t>(nodes_.size() / 2);
    switch (v.type()) {
      case Json::Type::Undefined:
      case Json::Type::Null: set_node(index, word0_of(Json::Type::Null, 0), 0); break;
      case Json::Type::Bool: set_node(index, word0_of(Json::Type::Bool, v.as_bool() ? 1 : 0), 0); break;
      case Json::Type::Number: {
        // -0 and 0 compare equal as map keys; keep them apart.
        const double d = v.as_number();
        uint32_t i;
        auto it = number_index.find(d);
        if (it != number_index.end() && std::signbit(it->first) == std::signbit(d)) {
          i = it->second;
        } else {
          i = static_cast<uint32_t>(numbers_.size());
          numbers_.push_back(d);
          if (it == number_index.end()) number_index.emplace(d, i);
        }
        set_node(index, word0_of(Json::Type::Number, 0), i);
        break;
      }
      case Json::Type::String: {
        const std::string& s = v.as_string();
        set_node(index, word0_of(Json::Type::String, s.size()), intern(s));
        break;
      }
      case Json::Type::Array: {
        set_node(index, word0_of(Json::Type::Array, v.size()), next);
        nodes_.resize(nodes_.size() + 2 * v.size(), 0);
        for (size_t i = 0; i < v.size(); ++i) queue.emplace_back(next + static_cast<uint32_t>(i), &v[i]);
        break;
      }
      case Json::Type::Object: {
        // Undefined members are left out, as Json::dump() does.
        size_t count = 0;
        for (const auto& m : v.as_object()) count += m.second.is_undefined() ? 0 : 1;
        set_node(index, word0_of(Json::Type::Object, count), next);
        nodes_.resize(nodes_.size() + 4 * count, 0);
        uint32_t slot = next;
        for (const auto& m : v.as_object()) {
          if (m.second.is_undefined()) continue;
          set_node(slot, word0_of(Json::Type::String, m.first.size()), intern(m.first));
          queue.emplace_back(slot + 1, &m.second);
          slot += 2;
        }
        break;
      }
    }
  }
  doc_.nodes = nodes_.data();
  doc_.strings = strings_.data();
  doc_.numbers = numbers_.data();
  doc_.node_count = static_cast<uint32_t>(nodes_.size() / 2);
}

size_t PackedBuffer::byte_size() const {
  return nodes_.size() * sizeof(uint32_t) + strings_.size() + numbers_.size() * sizeof(double);
}

}  // namespace a2ui

// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Read-only views of JSON schema documents, so the validator and the
// catalog can work on schemas that are not Json trees in RAM.
//
// A PackedDocument is a JSON document flattened into constant tables: it
// can live in flash and costs no heap. tools/embed_spec_documents.py
// generates the packed form of the specification documents
// (spec::packed()); PackedBuffer builds one at run time.
//
// A SchemaNode is a view of one value, either a Json value or a node of a
// PackedDocument. It does not own what it points to.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "a2ui/json.h"

namespace a2ui {

/**
 * A JSON document as constant tables. Node 0 is the root. Each node is two
 * words: the type (Json::Type) in the low 3 bits of the first word and a
 * count in the rest (string length, element or member count, or the bool
 * value); the second word is a string offset, a number index, or the index
 * of the first child. A container's children are consecutive nodes; an
 * object's are key, value, key, value, with keys as string nodes. Member
 * order is the document's.
 */
struct PackedDocument {
  const uint32_t* nodes;    // 2 words per node.
  const char* strings;      // String pool (not NUL-separated).
  const double* numbers;
  uint32_t node_count;
};

class SchemaNode {
 public:
  /** An absent node (a missing member or array element). */
  SchemaNode() = default;
  /** A view of `json`; absent when `json` is nullptr or an undefined value. */
  SchemaNode(const Json* json);  // NOLINT(google-explicit-constructor)
  SchemaNode(const Json& json) : SchemaNode(&json) {}  // NOLINT(google-explicit-constructor)
  /** Node `index` of `doc`. */
  SchemaNode(const PackedDocument* doc, uint32_t index) : doc_(doc), index_(index) {}

  /** The root of a packed document. */
  static SchemaNode root(const PackedDocument& doc) { return SchemaNode(&doc, 0); }

  /** Undefined when absent. */
  Json::Type type() const;
  bool present() const { return type() != Json::Type::Undefined; }
  explicit operator bool() const { return present(); }
  bool is_null() const { return type() == Json::Type::Null; }
  bool is_bool() const { return type() == Json::Type::Bool; }
  bool is_number() const { return type() == Json::Type::Number; }
  bool is_string() const { return type() == Json::Type::String; }
  bool is_array() const { return type() == Json::Type::Array; }
  bool is_object() const { return type() == Json::Type::Object; }

  /** True when this views a PackedDocument node. */
  bool is_packed() const { return doc_ != nullptr; }
  /** The viewed Json, or null for a packed node. */
  const Json* json() const { return json_; }

  bool as_bool(bool fallback = false) const;
  double as_number(double fallback = 0) const;
  /** The string, or empty for any other type. */
  std::string_view as_string() const;

  /** Element or member count; 0 for scalars. */
  size_t size() const;
  bool empty() const { return size() == 0; }

  /** Object member; absent when missing or when this is not an object. */
  SchemaNode find(std::string_view key) const;
  SchemaNode operator[](std::string_view key) const { return find(key); }
  SchemaNode operator[](const char* key) const { return find(key); }
  bool contains(std::string_view key) const { return find(key).present(); }
  /** Array element; absent when out of range. */
  SchemaNode at(size_t index) const;
  /** Key and value of member `index` of an object. */
  std::string_view key_at(size_t index) const;
  SchemaNode value_at(size_t index) const;

  /** Deep equality with a Json value, with Json's operator== rules. */
  bool equals(const Json& other) const;
  /** Deep equality of two nodes. */
  bool equals(const SchemaNode& other) const;

  /** A Json copy (allocates). Undefined when absent. */
  Json to_json() const;
  /** Compact JSON text, as Json::dump(). */
  std::string dump() const;

  struct Member;
  template <bool kMembers>
  class Range;
  // Range-for helpers: `for (SchemaNode e : node.items())` and
  // `for (const auto& m : node.members())` (m.key, m.value). Empty for
  // other types.
  Range<false> items() const;
  Range<true> members() const;

 private:
  uint32_t word0() const { return doc_->nodes[2 * index_]; }
  uint32_t word1() const { return doc_->nodes[2 * index_ + 1]; }

  const Json* json_ = nullptr;
  const PackedDocument* doc_ = nullptr;
  uint32_t index_ = 0;
};

struct SchemaNode::Member {
  std::string_view key;
  SchemaNode value;
};

template <bool kMembers>
class SchemaNode::Range {
 public:
  class Iterator {
   public:
    Iterator(const SchemaNode* n, size_t i) : n_(n), i_(i) {}
    auto operator*() const {
      if constexpr (kMembers) {
        return Member{n_->key_at(i_), n_->value_at(i_)};
      } else {
        return n_->at(i_);
      }
    }
    Iterator& operator++() {
      ++i_;
      return *this;
    }
    bool operator!=(const Iterator& o) const { return i_ != o.i_; }

   private:
    const SchemaNode* n_;
    size_t i_;
  };
  explicit Range(SchemaNode n) : n_(n), size_((kMembers ? n.is_object() : n.is_array()) ? n.size() : 0) {}
  Iterator begin() const { return Iterator(&n_, 0); }
  Iterator end() const { return Iterator(&n_, size_); }

 private:
  SchemaNode n_;
  size_t size_;
};

inline SchemaNode::Range<false> SchemaNode::items() const { return Range<false>(*this); }
inline SchemaNode::Range<true> SchemaNode::members() const { return Range<true>(*this); }

/** Resolves an RFC 6901 pointer (with URI %-decoding) inside `root`. Absent when it does not exist. */
SchemaNode resolve_node_pointer(SchemaNode root, std::string_view pointer);

/**
 * A PackedDocument built at run time from a Json value; owns its tables.
 * The packed specification documents need none of this (they are
 * generated); it is for catalogs that arrive at run time, and for tests.
 */
class PackedBuffer {
 public:
  explicit PackedBuffer(const Json& value);
  PackedBuffer(const PackedBuffer&) = delete;
  PackedBuffer& operator=(const PackedBuffer&) = delete;

  /** Valid while this buffer lives. */
  const PackedDocument& document() const { return doc_; }
  /** Bytes the tables take. */
  size_t byte_size() const;

 private:
  std::vector<uint32_t> nodes_;
  std::string strings_;
  std::vector<double> numbers_;
  PackedDocument doc_{};
};

}  // namespace a2ui

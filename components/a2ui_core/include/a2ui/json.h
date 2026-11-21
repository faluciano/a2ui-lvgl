// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// A small JSON value type for A2UI messages and data models.
//
// Design notes for microcontrollers:
// - No exceptions: parsing reports failure through a return value.
// - Objects keep insertion order in a vector. A2UI objects are small, so a
//   linear lookup beats a hash map in both RAM and flash.
// - `undefined` is a distinct value because the protocol distinguishes a
//   missing value from `null` (sparse array holes, absent arguments). It is
//   never produced by the parser and serializes as `null` inside arrays.

#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace a2ui {

class Json {
 public:
  enum class Type : uint8_t { Undefined, Null, Bool, Number, String, Array, Object };

  using Array = std::vector<Json>;
  using Member = std::pair<std::string, Json>;
  using Object = std::vector<Member>;

  Json() noexcept : type_(Type::Undefined) {}
  Json(std::nullptr_t) noexcept : type_(Type::Null) {}  // NOLINT(google-explicit-constructor)
  Json(bool b) noexcept : type_(Type::Bool) { u_.b = b; }  // NOLINT
  Json(double n) noexcept : type_(Type::Number) { u_.n = n; }  // NOLINT
  Json(int n) noexcept : Json(static_cast<double>(n)) {}  // NOLINT
  Json(long n) noexcept : Json(static_cast<double>(n)) {}  // NOLINT
  Json(long long n) noexcept : Json(static_cast<double>(n)) {}  // NOLINT
  Json(unsigned n) noexcept : Json(static_cast<double>(n)) {}  // NOLINT
  Json(unsigned long n) noexcept : Json(static_cast<double>(n)) {}  // NOLINT
  Json(unsigned long long n) noexcept : Json(static_cast<double>(n)) {}  // NOLINT
  Json(const char* s);  // NOLINT
  Json(std::string s);  // NOLINT
  Json(std::string_view s);  // NOLINT
  Json(Array a);  // NOLINT
  Json(Object o);  // NOLINT

  static Json undefined() { return Json(); }
  static Json null() { return Json(nullptr); }
  static Json array(std::initializer_list<Json> items = {});
  static Json object(std::initializer_list<Member> members = {});

  Json(const Json& other);
  Json(Json&& other) noexcept;
  Json& operator=(const Json& other);
  Json& operator=(Json&& other) noexcept;
  ~Json();

  Type type() const { return type_; }
  bool is_undefined() const { return type_ == Type::Undefined; }
  bool is_null() const { return type_ == Type::Null; }
  /** True for both null and undefined. */
  bool is_nullish() const { return type_ == Type::Null || type_ == Type::Undefined; }
  bool is_bool() const { return type_ == Type::Bool; }
  bool is_number() const { return type_ == Type::Number; }
  bool is_string() const { return type_ == Type::String; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_object() const { return type_ == Type::Object; }
  bool is_container() const { return is_array() || is_object(); }

  // Typed accessors. Calling the wrong one returns a neutral default rather
  // than aborting, because wire data is untrusted.
  bool as_bool(bool fallback = false) const { return is_bool() ? u_.b : fallback; }
  double as_number(double fallback = 0) const { return is_number() ? u_.n : fallback; }
  const std::string& as_string() const;
  const Array& as_array() const;
  const Object& as_object() const;
  Array& as_array_mut();    // Converts this value to an empty array first if needed.
  Object& as_object_mut();  // Converts this value to an empty object first if needed.

  /** Object member lookup; nullptr when absent or when this is not an object. */
  const Json* find(std::string_view key) const;
  Json* find_mut(std::string_view key);
  bool contains(std::string_view key) const { return find(key) != nullptr; }
  /** Object member or a shared undefined value. */
  const Json& operator[](std::string_view key) const;
  /** Array element or a shared undefined value. */
  const Json& operator[](size_t index) const;
  /** Inserts or replaces a member, keeping the original position on replace. */
  void set(std::string_view key, Json value);
  /** Removes a member; returns whether one was removed. */
  bool erase(std::string_view key);
  void push_back(Json value);
  /** Element or member count; 0 for scalars. */
  size_t size() const;
  bool empty() const { return size() == 0; }

  /** Deep structural equality. Numbers compare by value; object member order is ignored. */
  friend bool operator==(const Json& a, const Json& b);
  friend bool operator!=(const Json& a, const Json& b) { return !(a == b); }

  /** Serializes compactly. Undefined members are skipped; undefined elements print as null. */
  std::string dump() const;
  void dump_to(std::string& out) const;

  struct ParseResult;
  /** Parses RFC 8259 JSON. `max_depth` bounds recursion to protect small stacks. */
  static ParseResult parse(std::string_view text, int max_depth = 64);

 private:
  void destroy() noexcept;
  void copy_from(const Json& other);
  void move_from(Json&& other) noexcept;

  Type type_;
  union U {
    bool b;
    double n;
    std::string* s;
    Array* a;
    Object* o;
  } u_{};
};

struct Json::ParseResult {
  bool ok = false;
  Json value;
  std::string error;  // Human-readable reason when !ok.
  size_t offset = 0;  // Byte offset of the failure when !ok.
};

/**
 * Formats a number the way ECMAScript's Number.prototype.toString does
 * (shortest round-trip digits, exponent outside [1e-7, 1e21)). Used for the
 * protocol's "locale-neutral string representation" so every SDK agrees.
 */
std::string number_to_string(double value);

/** Appends `s` as a quoted JSON string literal. */
void append_json_string(std::string& out, std::string_view s);

}  // namespace a2ui

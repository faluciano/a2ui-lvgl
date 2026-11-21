// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0

#include "a2ui/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace a2ui {

namespace {

const std::string& empty_string() {
  static const std::string s;
  return s;
}
const Json::Array& empty_array() {
  static const Json::Array a;
  return a;
}
const Json::Object& empty_object() {
  static const Json::Object o;
  return o;
}
const Json& undefined_value() {
  static const Json u;
  return u;
}

}  // namespace

Json::Json(const char* s) : type_(Type::String) { u_.s = new std::string(s ? s : ""); }
Json::Json(std::string s) : type_(Type::String) { u_.s = new std::string(std::move(s)); }
Json::Json(std::string_view s) : type_(Type::String) { u_.s = new std::string(s); }
Json::Json(Array a) : type_(Type::Array) { u_.a = new Array(std::move(a)); }
Json::Json(Object o) : type_(Type::Object) { u_.o = new Object(std::move(o)); }

Json Json::array(std::initializer_list<Json> items) { return Json(Array(items)); }
Json Json::object(std::initializer_list<Member> members) {
  Json j(Object{});
  for (const auto& m : members) j.set(m.first, m.second);
  return j;
}

Json::Json(const Json& other) : type_(Type::Undefined) { copy_from(other); }
Json::Json(Json&& other) noexcept : type_(Type::Undefined) { move_from(std::move(other)); }
Json& Json::operator=(const Json& other) {
  if (this != &other) {
    Json tmp(other);  // Copy first: `other` may live inside *this.
    destroy();
    move_from(std::move(tmp));
  }
  return *this;
}
Json& Json::operator=(Json&& other) noexcept {
  if (this != &other) {
    Json tmp(std::move(other));  // `other` may live inside *this.
    destroy();
    move_from(std::move(tmp));
  }
  return *this;
}
Json::~Json() { destroy(); }

void Json::destroy() noexcept {
  switch (type_) {
    case Type::String: delete u_.s; break;
    case Type::Array: delete u_.a; break;
    case Type::Object: delete u_.o; break;
    default: break;
  }
  type_ = Type::Undefined;
  u_.s = nullptr;
}

void Json::copy_from(const Json& other) {
  type_ = other.type_;
  switch (type_) {
    case Type::String: u_.s = new std::string(*other.u_.s); break;
    case Type::Array: u_.a = new Array(*other.u_.a); break;
    case Type::Object: u_.o = new Object(*other.u_.o); break;
    default: u_ = other.u_; break;
  }
}

void Json::move_from(Json&& other) noexcept {
  type_ = other.type_;
  u_ = other.u_;
  other.type_ = Type::Undefined;
  other.u_.s = nullptr;
}

const std::string& Json::as_string() const { return is_string() ? *u_.s : empty_string(); }
const Json::Array& Json::as_array() const { return is_array() ? *u_.a : empty_array(); }
const Json::Object& Json::as_object() const { return is_object() ? *u_.o : empty_object(); }

Json::Array& Json::as_array_mut() {
  if (!is_array()) *this = Json(Array{});
  return *u_.a;
}
Json::Object& Json::as_object_mut() {
  if (!is_object()) *this = Json(Object{});
  return *u_.o;
}

const Json* Json::find(std::string_view key) const {
  if (!is_object()) return nullptr;
  for (const auto& m : *u_.o) {
    if (m.first == key) return &m.second;
  }
  return nullptr;
}

Json* Json::find_mut(std::string_view key) {
  if (!is_object()) return nullptr;
  for (auto& m : *u_.o) {
    if (m.first == key) return &m.second;
  }
  return nullptr;
}

const Json& Json::operator[](std::string_view key) const {
  const Json* v = find(key);
  return v ? *v : undefined_value();
}

const Json& Json::operator[](size_t index) const {
  if (!is_array() || index >= u_.a->size()) return undefined_value();
  return (*u_.a)[index];
}

void Json::set(std::string_view key, Json value) {
  Object& o = as_object_mut();
  for (auto& m : o) {
    if (m.first == key) {
      m.second = std::move(value);
      return;
    }
  }
  o.emplace_back(std::string(key), std::move(value));
}

bool Json::erase(std::string_view key) {
  if (!is_object()) return false;
  for (auto it = u_.o->begin(); it != u_.o->end(); ++it) {
    if (it->first == key) {
      u_.o->erase(it);
      return true;
    }
  }
  return false;
}

void Json::push_back(Json value) { as_array_mut().push_back(std::move(value)); }

size_t Json::size() const {
  if (is_array()) return u_.a->size();
  if (is_object()) return u_.o->size();
  return 0;
}

bool operator==(const Json& a, const Json& b) {
  if (a.type_ != b.type_) return false;
  switch (a.type_) {
    case Json::Type::Undefined:
    case Json::Type::Null: return true;
    case Json::Type::Bool: return a.u_.b == b.u_.b;
    case Json::Type::Number: return a.u_.n == b.u_.n;
    case Json::Type::String: return *a.u_.s == *b.u_.s;
    case Json::Type::Array: return *a.u_.a == *b.u_.a;
    case Json::Type::Object: {
      if (a.u_.o->size() != b.u_.o->size()) return false;
      for (const auto& m : *a.u_.o) {
        const Json* other = b.find(m.first);
        if (!other || !(m.second == *other)) return false;
      }
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Number formatting

std::string number_to_string(double v) {
  if (std::isnan(v)) return "NaN";
  if (std::isinf(v)) return v > 0 ? "Infinity" : "-Infinity";
  if (v == 0) return "0";  // Also covers -0, as in ECMAScript.

  // Fast path for integers that fit exactly.
  if (std::fabs(v) < 1e15 && v == std::floor(v)) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
    return buf;
  }

  // Shortest digit string that round-trips.
  char buf[40];
  int precision = 1;
  for (; precision <= 17; ++precision) {
    std::snprintf(buf, sizeof buf, "%.*e", precision - 1, v);
    if (std::strtod(buf, nullptr) == v) break;
  }

  // buf is "[-]d[.ddd]e[+-]xx". Split into sign, digits and exponent.
  std::string out;
  const char* p = buf;
  if (*p == '-') {
    out.push_back('-');
    ++p;
  }
  std::string digits;
  for (; *p && *p != 'e'; ++p) {
    if (*p != '.') digits.push_back(*p);
  }
  while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
  int exp10 = (*p == 'e') ? std::atoi(p + 1) : 0;
  const int k = static_cast<int>(digits.size());
  const int n = exp10 + 1;  // value = 0.digits * 10^n

  if (k <= n && n <= 21) {
    out += digits;
    out.append(static_cast<size_t>(n - k), '0');
  } else if (0 < n && n <= 21) {
    out += digits.substr(0, static_cast<size_t>(n));
    out.push_back('.');
    out += digits.substr(static_cast<size_t>(n));
  } else if (-6 < n && n <= 0) {
    out += "0.";
    out.append(static_cast<size_t>(-n), '0');
    out += digits;
  } else {
    out.push_back(digits[0]);
    if (k > 1) {
      out.push_back('.');
      out += digits.substr(1);
    }
    out.push_back('e');
    out.push_back(n - 1 >= 0 ? '+' : '-');
    out += std::to_string(std::abs(n - 1));
  }
  return out;
}

// ---------------------------------------------------------------------------
// Serialization

void append_json_string(std::string& out, std::string_view s) {
  static const char kHex[] = "0123456789abcdef";
  out.push_back('"');
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          out += "\\u00";
          out.push_back(kHex[c >> 4]);
          out.push_back(kHex[c & 0xF]);
        } else {
          out.push_back(ch);
        }
    }
  }
  out.push_back('"');
}

void Json::dump_to(std::string& out) const {
  switch (type_) {
    case Type::Undefined:
    case Type::Null: out += "null"; break;
    case Type::Bool: out += u_.b ? "true" : "false"; break;
    case Type::Number:
      if (std::isfinite(u_.n)) {
        out += number_to_string(u_.n);
      } else {
        out += "null";
      }
      break;
    case Type::String: append_json_string(out, *u_.s); break;
    case Type::Array: {
      out.push_back('[');
      bool first = true;
      for (const auto& e : *u_.a) {
        if (!first) out.push_back(',');
        first = false;
        e.dump_to(out);
      }
      out.push_back(']');
      break;
    }
    case Type::Object: {
      out.push_back('{');
      bool first = true;
      for (const auto& m : *u_.o) {
        if (m.second.is_undefined()) continue;
        if (!first) out.push_back(',');
        first = false;
        append_json_string(out, m.first);
        out.push_back(':');
        m.second.dump_to(out);
      }
      out.push_back('}');
      break;
    }
  }
}

std::string Json::dump() const {
  std::string out;
  dump_to(out);
  return out;
}

// ---------------------------------------------------------------------------
// Parsing

namespace {

class Parser {
 public:
  Parser(std::string_view text, int max_depth) : s_(text), max_depth_(max_depth) {}

  Json::ParseResult run() {
    Json::ParseResult r;
    skip_ws();
    if (!value(r.value, 0)) {
      r.error = error_;
      r.offset = pos_;
      return r;
    }
    skip_ws();
    if (pos_ != s_.size()) {
      r.error = "unexpected trailing characters";
      r.offset = pos_;
      r.value = Json();
      return r;
    }
    r.ok = true;
    return r;
  }

 private:
  bool fail(const char* msg) {
    error_ = msg;
    return false;
  }

  void skip_ws() {
    while (pos_ < s_.size()) {
      const char c = s_[pos_];
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
      ++pos_;
    }
  }

  bool literal(const char* word) {
    const size_t n = std::strlen(word);
    if (s_.compare(pos_, n, word) != 0) return fail("invalid literal");
    pos_ += n;
    return true;
  }

  bool value(Json& out, int depth) {
    if (pos_ >= s_.size()) return fail("unexpected end of input");
    switch (s_[pos_]) {
      case 'n': out = nullptr; return literal("null");
      case 't': out = true; return literal("true");
      case 'f': out = false; return literal("false");
      case '"': {
        std::string str;
        if (!string(str)) return false;
        out = Json(std::move(str));
        return true;
      }
      case '[': return array(out, depth + 1);
      case '{': return object(out, depth + 1);
      default: return number(out);
    }
  }

  bool array(Json& out, int depth) {
    if (depth > max_depth_) return fail("maximum nesting depth exceeded");
    ++pos_;  // '['
    Json::Array items;
    skip_ws();
    if (pos_ < s_.size() && s_[pos_] == ']') {
      ++pos_;
      out = Json(std::move(items));
      return true;
    }
    for (;;) {
      skip_ws();
      Json item;
      if (!value(item, depth)) return false;
      items.push_back(std::move(item));
      skip_ws();
      if (pos_ >= s_.size()) return fail("unterminated array");
      if (s_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (s_[pos_] == ']') {
        ++pos_;
        out = Json(std::move(items));
        return true;
      }
      return fail("expected ',' or ']'");
    }
  }

  bool object(Json& out, int depth) {
    if (depth > max_depth_) return fail("maximum nesting depth exceeded");
    ++pos_;  // '{'
    Json obj{Json::Object{}};
    skip_ws();
    if (pos_ < s_.size() && s_[pos_] == '}') {
      ++pos_;
      out = std::move(obj);
      return true;
    }
    for (;;) {
      skip_ws();
      if (pos_ >= s_.size() || s_[pos_] != '"') return fail("expected object key");
      std::string key;
      if (!string(key)) return false;
      skip_ws();
      if (pos_ >= s_.size() || s_[pos_] != ':') return fail("expected ':'");
      ++pos_;
      skip_ws();
      Json v;
      if (!value(v, depth)) return false;
      obj.set(key, std::move(v));  // Last duplicate wins, as in JSON.parse.
      skip_ws();
      if (pos_ >= s_.size()) return fail("unterminated object");
      if (s_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (s_[pos_] == '}') {
        ++pos_;
        out = std::move(obj);
        return true;
      }
      return fail("expected ',' or '}'");
    }
  }

  static void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool hex4(uint32_t& out) {
    if (pos_ + 4 > s_.size()) return fail("truncated \\u escape");
    out = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = s_[pos_++];
      out <<= 4;
      if (c >= '0' && c <= '9') {
        out |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        out |= static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        out |= static_cast<uint32_t>(c - 'A' + 10);
      } else {
        return fail("invalid \\u escape");
      }
    }
    return true;
  }

  bool string(std::string& out) {
    ++pos_;  // opening quote
    for (;;) {
      if (pos_ >= s_.size()) return fail("unterminated string");
      const char c = s_[pos_++];
      if (c == '"') return true;
      if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= s_.size()) return fail("unterminated escape");
      const char e = s_[pos_++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t cp;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            // High surrogate: combine with a following low surrogate if present.
            if (pos_ + 6 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
              const size_t save = pos_;
              pos_ += 2;
              uint32_t lo;
              if (!hex4(lo)) return false;
              if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              } else {
                pos_ = save;
                cp = 0xFFFD;
              }
            } else {
              cp = 0xFFFD;
            }
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;  // Lone low surrogate has no UTF-8 form.
          }
          append_utf8(out, cp);
          break;
        }
        default: return fail("invalid escape");
      }
    }
  }

  bool number(Json& out) {
    const size_t start = pos_;
    if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
    if (pos_ >= s_.size()) return fail("invalid number");
    if (s_[pos_] == '0') {
      ++pos_;
    } else if (s_[pos_] >= '1' && s_[pos_] <= '9') {
      while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
    } else {
      return fail("unexpected character");
    }
    if (pos_ < s_.size() && s_[pos_] == '.') {
      ++pos_;
      const size_t frac = pos_;
      while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
      if (pos_ == frac) return fail("invalid number");
    }
    if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
      const size_t exp = pos_;
      while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
      if (pos_ == exp) return fail("invalid number");
    }
    const std::string text(s_.substr(start, pos_ - start));
    out = std::strtod(text.c_str(), nullptr);
    return true;
  }

  std::string_view s_;
  size_t pos_ = 0;
  int max_depth_;
  const char* error_ = "";
};

}  // namespace

Json::ParseResult Json::parse(std::string_view text, int max_depth) {
  return Parser(text, max_depth).run();
}

}  // namespace a2ui

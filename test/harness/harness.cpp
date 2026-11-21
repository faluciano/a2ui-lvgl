// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0

#include "harness.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>

namespace harness {

std::map<std::string, ActionFn>& action_registry() {
  static std::map<std::string, ActionFn> registry;
  return registry;
}

ActionRegistrar::ActionRegistrar(const char* action, ActionFn fn) { action_registry()[action] = fn; }

std::string read_file(const std::string& path, bool* ok) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (ok) *ok = false;
    return {};
  }
  std::stringstream ss;
  ss << in.rdbuf();
  if (ok) *ok = true;
  return ss.str();
}

Json load_json_file(const std::string& path, std::string* error) {
  bool ok = false;
  const std::string text = read_file(path, &ok);
  if (!ok) {
    if (error) *error = "cannot read " + path;
    return Json();
  }
  auto r = Json::parse(text, 256);
  if (!r.ok) {
    if (error) *error = path + ": " + r.error + " at offset " + std::to_string(r.offset);
    return Json();
  }
  return std::move(r.value);
}

const Json* find_expect_error(const Json& node) {
  if (const Json* e = node.find("expectError")) return e;
  return node.find("expect_error");
}

bool regex_search(const std::string& pattern, const std::string& text) {
  try {
    return std::regex_search(text, std::regex(pattern, std::regex::ECMAScript));
  } catch (const std::regex_error&) {
    // Fall back to a substring test for patterns std::regex cannot handle.
    return text.find(pattern) != std::string::npos;
  }
}

static bool category_matches(const a2ui::Error& error, const std::string& category) {
  using a2ui::ErrorKind;
  static const std::map<std::string, ErrorKind> kinds = {
      {"ValidationError", ErrorKind::Validation}, {"IntegrityError", ErrorKind::Integrity},
      {"RecursionError", ErrorKind::Recursion},   {"CatalogError", ErrorKind::Catalog},
      {"DataError", ErrorKind::Data},             {"ExpressionError", ErrorKind::Expression},
      {"StateError", ErrorKind::State},           {"ParseError", ErrorKind::Parse},
      {"RpcError", ErrorKind::Rpc},               {"Error", ErrorKind::Generic},
  };
  auto it = kinds.find(category);
  if (it == kinds.end()) return false;
  return error.is(it->second);
}

Outcome check_error(const a2ui::Error& error, const Json& expect) {
  const std::string got = std::string(a2ui::error_kind_name(error.kind)) + ": " + error.message;
  if (const Json* cat = expect.find("category"); cat && cat->is_string()) {
    if (!category_matches(error, cat->as_string())) {
      return Outcome::fail("expected " + cat->as_string() + ", got " + got);
    }
  }
  if (const Json* msg = expect.find("message"); msg && msg->is_string()) {
    // As the reference harnesses do: a case-insensitive substring match
    // first (messages such as "Unsupported external $ref" are not meant as
    // regexes), then a regex search.
    auto lower = [](std::string t) {
      for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return t;
    };
    const bool substring = lower(error.message).find(lower(msg->as_string())) != std::string::npos;
    if (!substring && !regex_search(msg->as_string(), error.message)) {
      return Outcome::fail("error message /" + msg->as_string() + "/ did not match: " + got);
    }
  }
  struct Field {
    const char* key;
    const std::string* value;
  };
  const Field fields[] = {{"code", &error.code},
                          {"surfaceId", &error.surface_id},
                          {"path", &error.path},
                          {"functionCallId", &error.function_call_id}};
  for (const auto& f : fields) {
    const Json* want = expect.find(f.key);
    if (want && want->is_string() && want->as_string() != *f.value) {
      return Outcome::fail(std::string("error ") + f.key + " expected '" + want->as_string() +
                           "', got '" + *f.value + "' (" + got + ")");
    }
  }
  return Outcome::pass();
}

Outcome missing_error(const Json& expect) {
  return Outcome::fail("expected error " + expect.dump() + " but the call succeeded");
}

static bool is_match_any(const Json& j) {
  const Json* m = j.find("$match");
  return j.size() == 1 && m && m->is_string() && m->as_string() == "any";
}

std::string json_mismatch(const Json& expected, const Json& actual, const std::string& path) {
  const std::string where = path.empty() ? "/" : path;
  if (expected.is_object() && is_match_any(expected)) {
    return actual.is_nullish() ? where + ": expected any value, got " + actual.dump() : "";
  }
  if (expected.is_object() && actual.is_object()) {
    for (const auto& m : expected.as_object()) {
      const Json* a = actual.find(m.first);
      if (!a) {
        if (m.second.is_undefined()) continue;
        return path + "/" + m.first + ": missing (expected " + m.second.dump() + ")";
      }
      std::string r = json_mismatch(m.second, *a, path + "/" + m.first);
      if (!r.empty()) return r;
    }
    for (const auto& m : actual.as_object()) {
      if (!expected.find(m.first) && !m.second.is_undefined()) {
        return path + "/" + m.first + ": unexpected member " + m.second.dump();
      }
    }
    return "";
  }
  if (expected.is_array() && actual.is_array()) {
    if (expected.size() != actual.size()) {
      return where + ": expected " + std::to_string(expected.size()) + " elements, got " +
             std::to_string(actual.size()) + " (" + actual.dump() + ")";
    }
    for (size_t i = 0; i < expected.size(); ++i) {
      std::string r = json_mismatch(expected[i], actual[i], path + "/" + std::to_string(i));
      if (!r.empty()) return r;
    }
    return "";
  }
  if (expected == actual) return "";
  return where + ": expected " + expected.dump() + ", got " + actual.dump();
}

static bool is_pre_v1_version(const Json& v) {
  if (!v.is_string()) return false;
  const std::string& s = v.as_string();
  return s.rfind("v0.", 0) == 0 || s.rfind("0.", 0) == 0;
}

static bool scan_pre_v1(const Json& node, int depth) {
  if (depth > 8) return false;
  if (node.is_object()) {
    for (const auto& m : node.as_object()) {
      if ((m.first == "version" || m.first == "protocolVersion") && is_pre_v1_version(m.second)) {
        return true;
      }
      if (m.second.is_container() && scan_pre_v1(m.second, depth + 1)) return true;
    }
  } else if (node.is_array()) {
    for (const auto& e : node.as_array()) {
      if (scan_pre_v1(e, depth + 1)) return true;
    }
  }
  return false;
}

bool targets_pre_v1(const Json& test_case) { return scan_pre_v1(test_case, 0); }

}  // namespace harness

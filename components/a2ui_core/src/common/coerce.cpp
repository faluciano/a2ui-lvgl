// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0

#include "a2ui/coerce.h"

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace a2ui {

bool is_validation_result(const Json& v) {
  const Json* valid = v.find("valid");
  return valid && valid->is_bool();
}

bool coerce_bool(const Json& v) {
  switch (v.type()) {
    case Json::Type::Bool: return v.as_bool();
    case Json::Type::Number: return v.as_number() != 0 && !std::isnan(v.as_number());
    case Json::Type::String: {
      std::string s = v.as_string();
      for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return s == "true";
    }
    case Json::Type::Object: return is_validation_result(v) && v["valid"].as_bool();
    default: return false;
  }
}

std::string coerce_string(const Json& v) {
  switch (v.type()) {
    case Json::Type::Undefined:
    case Json::Type::Null: return "";
    case Json::Type::Bool: return v.as_bool() ? "true" : "false";
    case Json::Type::Number: return number_to_string(v.as_number());
    case Json::Type::String: return v.as_string();
    default: return v.dump();
  }
}

double coerce_number(const Json& v) {
  switch (v.type()) {
    case Json::Type::Number: return v.as_number();
    case Json::Type::Bool: return v.as_bool() ? 1 : 0;
    case Json::Type::String: {
      const std::string& s = v.as_string();
      size_t b = 0, e = s.size();
      while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
      while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
      if (b == e) return 0;
      const std::string t = s.substr(b, e - b);
      char* end = nullptr;
      const double d = std::strtod(t.c_str(), &end);
      if (end != t.c_str() + t.size() || std::isnan(d)) return 0;
      return d;
    }
    default: return 0;
  }
}

}  // namespace a2ui

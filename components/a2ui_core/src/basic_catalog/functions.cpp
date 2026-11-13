// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Basic catalog function implementations (v1.0). Validation functions return
// ValidationResult objects ({"valid": bool}); see the basic catalog guide.

#include <cctype>
#include <cmath>
#include <map>

#include "a2ui/basic_catalog.h"
#include "a2ui/coerce.h"
#include "a2ui/expressions.h"
#include "a2ui/identifier.h"
#include "a2ui/locale.h"
#include "a2ui/regex.h"

namespace a2ui {
namespace basic_catalog {

namespace {

Json validation_result(bool valid) { return Json::object({{"valid", valid}}); }

Error expression_error(const std::string& msg) { return make_error(ErrorKind::Expression, msg); }

Result<Json> fn_required(const Json& args, FunctionContext&) {
  const Json& v = args["value"];
  bool ok = !v.is_nullish();
  if (v.is_string() && v.as_string().empty()) ok = false;
  if (v.is_array() && v.empty()) ok = false;
  return validation_result(ok);
}

Result<Json> fn_regex(const Json& args, FunctionContext&) {
  const std::string value = coerce_string(args["value"]);
  if (utf8_length(value) > regex::kMaxValueCodePoints) return validation_result(false);
  auto pattern = regex::Pattern::compile(args["pattern"].as_string());
  if (!pattern.ok()) return pattern.take_error();
  return validation_result(pattern.value().search(value));
}

Result<Json> fn_length(const Json& args, FunctionContext&) {
  const Json& v = args["value"];
  double len;
  if (v.is_array()) {
    len = static_cast<double>(v.size());
  } else {
    len = static_cast<double>(utf8_length(coerce_string(v)));
  }
  bool ok = true;
  if (args["min"].is_number() && len < args["min"].as_number()) ok = false;
  if (args["max"].is_number() && len > args["max"].as_number()) ok = false;
  return validation_result(ok);
}

Result<Json> fn_numeric(const Json& args, FunctionContext&) {
  const Json& v = args["value"];
  double n;
  if (v.is_number()) {
    n = v.as_number();
  } else if (v.is_string()) {
    const std::string& s = v.as_string();
    char* end = nullptr;
    n = std::strtod(s.c_str(), &end);
    if (s.empty() || end != s.c_str() + s.size()) return validation_result(false);
  } else {
    return validation_result(false);
  }
  if (std::isnan(n)) return validation_result(false);
  bool ok = true;
  if (args["min"].is_number() && n < args["min"].as_number()) ok = false;
  if (args["max"].is_number() && n > args["max"].as_number()) ok = false;
  return validation_result(ok);
}

/** ^[^\s@]+@[^\s@]+\.[^\s@]{2,}$ style check, matching the reference SDKs. */
Result<Json> fn_email(const Json& args, FunctionContext&) {
  const std::string s = coerce_string(args["value"]);
  auto bad = [](char c) { return c == '@' || std::isspace(static_cast<unsigned char>(c)); };
  const size_t at = s.find('@');
  if (at == std::string::npos || at == 0 || s.find('@', at + 1) != std::string::npos) {
    return validation_result(false);
  }
  for (char c : s) {
    if (c != '@' && bad(c)) return validation_result(false);
  }
  const std::string domain = s.substr(at + 1);
  const size_t dot = domain.rfind('.');
  if (dot == std::string::npos || dot == 0 || domain.size() - dot - 1 < 2) return validation_result(false);
  return validation_result(true);
}

Result<Json> logical(const Json& args, bool is_and) {
  const auto& values = args["values"].as_array();
  if (values.size() < 2) {
    return expression_error(std::string(is_and ? "and" : "or") + " requires at least two values");
  }
  for (const auto& v : values) {
    const bool b = coerce_bool(v);
    if (is_and && !b) return Json(false);
    if (!is_and && b) return Json(true);
  }
  return Json(is_and);
}

Result<Json> fn_and(const Json& args, FunctionContext&) { return logical(args, true); }
Result<Json> fn_or(const Json& args, FunctionContext&) { return logical(args, false); }
Result<Json> fn_not(const Json& args, FunctionContext&) { return Json(!coerce_bool(args["value"])); }

Result<Json> fn_open_url(const Json& args, FunctionContext& ctx) {
  const std::string url = coerce_string(args["url"]);
  if (!is_allowed_open_url(url)) {
    return expression_error("openUrl only opens absolute http or https URLs");
  }
  auto s = ctx.open_url(url);
  if (!s.ok()) return s.take_error();
  return Json(nullptr);
}

locale::NumberOptions number_options(const Json& args) {
  locale::NumberOptions o;
  if (args["decimals"].is_number()) o.decimals = static_cast<int>(args["decimals"].as_number());
  if (!args["grouping"].is_nullish()) {
    o.grouping = coerce_bool(args["grouping"]);
    o.locale_min_grouping = false;  // Intl: an explicit grouping means "always".
  }
  return o;
}

// Formatting failures render as "" in every reference SDK (see locale.h).
Json text_or_empty(const Result<std::string>& r) { return Json(r.ok() ? r.value() : std::string()); }

Result<Json> fn_format_number(const Json& args, FunctionContext&) {
  if (args["value"].is_nullish()) return Json("");
  return text_or_empty(locale::format_number(coerce_number(args["value"]), number_options(args),
                                             args["locale"].as_string()));
}

Result<Json> fn_format_currency(const Json& args, FunctionContext&) {
  if (args["value"].is_nullish()) return Json("");
  return text_or_empty(locale::format_currency(coerce_number(args["value"]),
                                               args["currency"].as_string(), number_options(args),
                                               args["locale"].as_string()));
}

Result<Json> fn_format_date(const Json& args, FunctionContext&) {
  const Json& v = args["value"];
  if (v.is_nullish()) return Json("");
  const std::string fmt = args["format"].as_string();
  return text_or_empty(v.is_number()
                           ? locale::format_date_millis(v.as_number(), fmt, args["locale"].as_string())
                           : locale::format_date(coerce_string(v), fmt, args["locale"].as_string()));
}

Result<Json> fn_pluralize(const Json& args, FunctionContext&) {
  static const struct {
    const char* key;
    unsigned bit;
  } kForms[] = {{"zero", locale::kPluralZero}, {"one", locale::kPluralOne},
                {"two", locale::kPluralTwo},   {"few", locale::kPluralFew},
                {"many", locale::kPluralMany}, {"other", locale::kPluralOther}};
  unsigned present = 0;
  for (const auto& f : kForms) {
    if (!args[f.key].is_nullish()) present |= f.bit;
  }
  const char* key = locale::pluralize_key(coerce_number(args["value"]), present,
                                          args["locale"].as_string());
  return Json(coerce_string(args[key]));
}

/** Converts a version-neutral template part into a v1.0 dynamic value. */
Json to_v10_dynamic(const Json& part, const std::string& catalog_id) {
  if (!part.is_object()) return part;
  if (const Json* p = part.find("path")) return Json::object({{"@path", *p}});
  if (const Json* c = part.find("call")) {
    Json out = Json::object({{"@call", *c}});
    Json args = Json::object();
    for (const auto& m : part["args"].as_object()) args.set(m.first, to_v10_dynamic(m.second, catalog_id));
    out.set("args", std::move(args));
    const std::string& name = c->as_string();
    if (!catalog_id.empty() && !(name.size() > 0 && name[0] == '@')) out.set("catalogId", catalog_id);
    return out;
  }
  return part;
}

Result<Json> fn_format_string(const Json& args, FunctionContext& ctx) {
  const Json& value = args["value"];
  if (!value.is_string()) return Json(coerce_string(value));
  auto parts = expressions::parse_template(value.as_string());
  if (!parts.ok()) return parts.take_error();
  std::string out;
  const std::string catalog = ctx.active_catalog_id();
  for (const auto& part : parts.value().as_array()) {
    if (part.is_string()) {
      out += part.as_string();
      continue;
    }
    auto r = ctx.resolve(to_v10_dynamic(part, catalog));
    if (!r.ok()) return r.take_error();
    out += coerce_string(r.value());
  }
  return Json(std::move(out));
}

const std::map<std::string, FunctionFn>& table() {
  static const std::map<std::string, FunctionFn> t = {
      {"required", fn_required},         {"regex", fn_regex},
      {"length", fn_length},             {"numeric", fn_numeric},
      {"email", fn_email},               {"formatString", fn_format_string},
      {"formatNumber", fn_format_number}, {"formatCurrency", fn_format_currency},
      {"formatDate", fn_format_date},    {"pluralize", fn_pluralize},
      {"openUrl", fn_open_url},          {"and", fn_and},
      {"or", fn_or},                     {"not", fn_not},
  };
  return t;
}

bool starts_with_ci(const std::string& s, const char* prefix) {
  size_t i = 0;
  for (; prefix[i]; ++i) {
    if (i >= s.size() || std::tolower(static_cast<unsigned char>(s[i])) != prefix[i]) return false;
  }
  return true;
}

}  // namespace

FunctionFn function_impl(const std::string& name) {
  auto it = table().find(name);
  return it == table().end() ? FunctionFn() : it->second;
}

void bind_functions(Catalog& catalog) {
  for (const auto& entry : table()) {
    if (catalog.find_function(entry.first)) (void)catalog.bind_function(entry.first, entry.second);
  }
}

bool is_allowed_open_url(const std::string& raw) {
  std::string url = raw;
  while (!url.empty() && static_cast<unsigned char>(url.front()) <= 0x20) url.erase(0, 1);
  while (!url.empty() && static_cast<unsigned char>(url.back()) <= 0x20) url.pop_back();
  size_t prefix;
  if (starts_with_ci(url, "https://")) {
    prefix = 8;
  } else if (starts_with_ci(url, "http://")) {
    prefix = 7;
  } else {
    return false;
  }
  return url.size() > prefix && url[prefix] != '/';
}

bool is_allowed_media_url(const std::string& raw) {
  // 1. Trim whitespace and control characters.
  size_t b = 0, e = raw.size();
  while (b < e && static_cast<unsigned char>(raw[b]) <= 0x20) ++b;
  while (e > b && static_cast<unsigned char>(raw[e - 1]) <= 0x20) --e;
  if (b == e) return false;
  // Inspect only a bounded prefix (data: URLs can be huge), dropping tab/CR/LF as browsers do.
  std::string head;
  for (size_t i = b; i < e && head.size() < 64; ++i) {
    const char c = raw[i];
    if (c == '\t' || c == '\n' || c == '\r') continue;
    head.push_back(c == '\\' ? '/' : c);
  }
  // 2. Find a scheme: letters followed by ':' before any '/', '?' or '#'.
  size_t colon = std::string::npos;
  for (size_t i = 0; i < head.size(); ++i) {
    const char c = head[i];
    if (c == ':') {
      colon = i;
      break;
    }
    if (c == '/' || c == '?' || c == '#') break;
  }
  if (colon == std::string::npos) {
    // 4. Relative path: reject protocol-relative "//host".
    return !(head.size() >= 2 && head[0] == '/' && head[1] == '/');
  }
  std::string scheme = head.substr(0, colon);
  for (auto& c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (scheme == "data") return true;
  if (scheme != "http" && scheme != "https") return false;
  // 3. Require "//" and a non-empty host.
  return head.size() > colon + 3 && head[colon + 1] == '/' && head[colon + 2] == '/' &&
         head[colon + 3] != '/';
}

}  // namespace basic_catalog
}  // namespace a2ui

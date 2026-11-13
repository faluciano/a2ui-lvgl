// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Port of swift/core/Sources/A2UICore/State/DataContext.swift (v1.0 paths).

#include "a2ui/data_context.h"

#include <cmath>

#include "a2ui/coerce.h"

namespace a2ui {

namespace function_errors {
Error not_found(const std::string& name) {
  return make_error(ErrorKind::Expression, "Function '" + name + "' not found", "EXPRESSION_ERROR");
}
Error caller_disallowed(const std::string& name) {
  return make_error(ErrorKind::Expression,
                    "Function '" + name + "' cannot be called by renderer (allowedCallers is agentOnly).",
                    "EXPRESSION_ERROR");
}
Error user_activation_required(const std::string& name) {
  return make_error(ErrorKind::Expression,
                    "Function '" + name +
                        "' requires user activation and cannot be evaluated without an active user gesture.",
                    "EXPRESSION_ERROR");
}
Error invalid_arguments(const std::string& name) {
  return make_error(ErrorKind::Validation,
                    "Validation failed for function '" + name + "': the arguments do not match its schema.",
                    "EXPRESSION_ERROR");
}
}  // namespace function_errors

namespace {

bool ref_ends_with(const Json& schema, const char* suffix) {
  const Json* ref = schema.find("$ref");
  if (!ref || !ref->is_string()) return false;
  const std::string& r = ref->as_string();
  const std::string s(suffix);
  return r.size() >= s.size() && r.compare(r.size() - s.size(), s.size(), s) == 0;
}

bool is_dynamic_expression_branch(const Json& schema) {
  if (schema.contains("$ref")) {
    return ref_ends_with(schema, "/$defs/DataBinding") || ref_ends_with(schema, "/$defs/FunctionCall");
  }
  const Json* props = schema.find("properties");
  if (props && props->is_object() && !props->contains("componentId")) {
    return props->contains("@path") || props->contains("path") || props->contains("@call") ||
           props->contains("call");
  }
  return false;
}

bool is_dynamic_boolean_schema(const Json& schema) {
  if (ref_ends_with(schema, "/$defs/DynamicBoolean")) return true;
  if (schema["type"].is_string() && schema["type"].as_string() == "boolean") return true;
  for (const auto& s : schema["allOf"].as_array()) {
    if (is_dynamic_boolean_schema(s)) return true;
  }
  for (const char* kw : {"oneOf", "anyOf"}) {
    const auto& subs = schema[kw].as_array();
    if (subs.empty()) continue;
    bool has_bool = false, only = true;
    for (const auto& s : subs) {
      const bool b = is_dynamic_boolean_schema(s);
      has_bool = has_bool || b;
      const bool is_null = s["type"].is_string() && s["type"].as_string() == "null";
      if (!(b || is_null || is_dynamic_expression_branch(s))) only = false;
    }
    if (has_bool && only) return true;
  }
  return false;
}

bool is_json_representable(const Json& v) {
  switch (v.type()) {
    case Json::Type::Number: return std::isfinite(v.as_number());
    case Json::Type::Array:
      for (const auto& e : v.as_array()) {
        if (!is_json_representable(e)) return false;
      }
      return true;
    case Json::Type::Object:
      for (const auto& m : v.as_object()) {
        if (!is_json_representable(m.second)) return false;
      }
      return true;
    default: return true;
  }
}

Json apply_defaults(const Json& schema, const Json& args) {
  const Json* props = schema.find("properties");
  if (!props || !props->is_object() || !args.is_object()) return args;
  Json out = args;
  for (const auto& m : props->as_object()) {
    Json* existing = out.find_mut(m.first);
    if (existing) {
      if (existing->is_object()) *existing = apply_defaults(m.second, *existing);
    } else if (const Json* def = m.second.find("default")) {
      out.set(m.first, *def);
    }
  }
  return out;
}

/** "@@x" -> "@x" (escaped keys in plain objects). */
std::string unescape_key(const std::string& k) {
  if (k.size() >= 2 && k[0] == '@' && k[1] == '@') return k.substr(1);
  return k;
}

}  // namespace

Json coerce_validation_results(const Json& schema, const Json& value) {
  if (is_dynamic_boolean_schema(schema)) return Json(coerce_bool(value));
  if (const Json* items = schema.find("items"); items && value.is_array()) {
    Json out{Json::Array{}};
    for (const auto& e : value.as_array()) out.push_back(coerce_validation_results(*items, e));
    return out;
  }
  if (const Json* props = schema.find("properties"); props && props->is_object() && value.is_object()) {
    Json out = Json::object();
    for (const auto& m : value.as_object()) {
      const Json* ps = props->find(m.first);
      out.set(m.first, ps ? coerce_validation_results(*ps, m.second) : m.second);
    }
    return out;
  }
  return value;
}

DataContext::DataContext(DataModel& model, FunctionHost* host, std::string p,
                         std::optional<long> index, bool user_activated)
    : model_(&model),
      host_(host),
      path_(path::normalize(p)),
      index_(index),
      user_activated_(user_activated) {}

bool DataContext::is_invalid_reserved_key(const std::string& key) {
  if (key.empty() || key[0] != '@') return false;
  if (key.size() >= 2 && key[1] == '@') return false;
  return key != "@path" && key != "@call";
}

DataContext DataContext::nested(const std::string& relative_path, std::optional<long> index) const {
  DataContext c(*model_, host_, path::resolve(relative_path, path_), index ? index : index_,
                user_activated_);
  c.deps_ = deps_;
  return c;
}

Status DataContext::set(const std::string& p, Json value) {
  return model_->set(absolute_path(p), std::move(value));
}

Status DataContext::open_url(const std::string& url) {
  if (!host_) return make_error(ErrorKind::Generic, "No URL handler is configured");
  return host_->open_url(url);
}

void DataContext::fail(Error error, const std::string& function_name) {
  failed_ = true;
  if (host_) host_->report_function_error(error, function_name);
}

Json DataContext::resolve_value(const Json& value, bool user_activated) {
  if (user_activated == user_activated_) return resolve_value(value);
  DataContext scoped(*model_, host_, path_, index_, user_activated);
  scoped.active_catalog_id_ = active_catalog_id_;
  scoped.deps_ = deps_;
  Json r = scoped.resolve_value(value);
  failed_ = failed_ || scoped.failed_;
  deferred_ = deferred_ || scoped.deferred_;
  deferred_arguments_ = deferred_arguments_ || scoped.deferred_arguments_;
  return r;
}

Json DataContext::resolve_value(const Json& value) {
  switch (value.type()) {
    case Json::Type::Array: {
      Json out{Json::Array{}};
      for (const auto& e : value.as_array()) out.push_back(resolve_value(e));
      return out;
    }
    case Json::Type::Object: {
      if (const Json* p = value.find("@path"); p && p->is_string()) {
        const std::string abs = absolute_path(p->as_string());
        if (deps_) deps_->push_back(abs);
        Json v = model_->value_at(abs);
        return v.is_undefined() ? Json(nullptr) : v;
      }
      if (const Json* c = value.find("@call"); c && c->is_string()) {
        return evaluate_call(c->as_string(), value);
      }
      Json out = Json::object();
      for (const auto& m : value.as_object()) out.set(unescape_key(m.first), resolve_value(m.second));
      return out;
    }
    default: return value;
  }
}

bool DataContext::resolve_arguments(const Json& call, Json* out) {
  const bool deferred_before = deferred_;
  const bool failed_before = failed_;
  deferred_ = false;
  failed_ = false;
  Json args = Json::object();
  for (const auto& m : call["args"].as_object()) args.set(m.first, resolve_value(m.second));
  const bool unsettled = deferred_ || failed_;
  deferred_ = deferred_ || deferred_before;
  failed_ = failed_ || failed_before;
  if (unsettled) {
    deferred_ = true;
    deferred_arguments_ = true;
    return false;
  }
  *out = std::move(args);
  return true;
}

Json DataContext::evaluate_call(const std::string& name, const Json& call) {
  const Json* raw_catalog = call.find("catalogId");

  // System functions ('@' prefix) are catalog-less.
  if (!name.empty() && name[0] == '@') {
    if (raw_catalog) {
      fail(make_error(ErrorKind::Expression, "System function " + name +
                                                 " does not accept a catalogId (got " +
                                                 raw_catalog->dump() + ")"),
           name);
      return nullptr;
    }
    if (name != "@index") {
      fail(function_errors::not_found(name), name);
      return nullptr;
    }
    if (!index_) {
      fail(make_error(ErrorKind::Expression,
                      "@index function can only be evaluated inside a collection template iteration scope."),
           name);
      return nullptr;
    }
    Json args;
    if (!resolve_arguments(call, &args)) return nullptr;
    const double offset = args.contains("offset") ? coerce_number(args["offset"]) : 0.0;
    return Json(static_cast<double>(*index_) + offset);
  }

  if (raw_catalog && !raw_catalog->is_string()) {
    Error e = make_error(ErrorKind::Catalog, "Catalog " + raw_catalog->dump() + " is not available");
    e.path = "/@call/catalogId";
    fail(std::move(e), name);
    return nullptr;
  }
  if (!host_) {
    fail(function_errors::not_found(name), name);
    return nullptr;
  }
  const std::string* catalog_id = raw_catalog ? &raw_catalog->as_string() : nullptr;
  auto catalog = host_->resolve_function_catalog(catalog_id, name);
  if (!catalog.ok()) {
    fail(catalog.take_error(), name);
    return nullptr;
  }
  const Catalog* cat = catalog.value();
  const FunctionDecl* decl = cat->find_function(name);

  if (decl && decl->api.allowed_callers == AllowedCallers::AgentOnly) {
    fail(function_errors::caller_disallowed(name), name);
    return nullptr;
  }
  if (!decl || !decl->has_local_impl()) {
    if (host_->routes_to_agent()) return route_to_agent(name, decl, cat->id(), call);
    fail(function_errors::not_found(name), name);
    return nullptr;
  }
  if (decl->api.allowed_callers == AllowedCallers::AgentOnly) {
    fail(function_errors::caller_disallowed(name), name);
    return nullptr;
  }
  if (decl->api.requires_user_activation && !user_activated_) {
    fail(function_errors::user_activation_required(name), name);
    return nullptr;
  }
  Json args;
  if (!resolve_arguments(call, &args)) return nullptr;
  args = coerce_validation_results(decl->api.args_schema, args);
  if (auto v = host_->validate_function_args(decl->api, args); !v.ok()) {
    fail(function_errors::invalid_arguments(name), name);
    return nullptr;
  }

  const std::string previous_catalog = active_catalog_id_;
  active_catalog_id_ = cat->id();
  ++running_function_depth_;
  auto result = decl->impl(args, *this);
  --running_function_depth_;
  active_catalog_id_ = previous_catalog;
  if (!result.ok()) {
    fail(result.take_error(), name);
    return nullptr;
  }
  return std::move(result).value();
}

Json DataContext::route_to_agent(const std::string& name, const FunctionDecl* decl,
                                 const std::string& catalog_id, const Json& call) {
  if (running_function_depth_ > 0) {
    fail(make_error(ErrorKind::Expression,
                    "Function '" + name +
                        "' runs on the agent, and a template part can't wait for it."),
         name);
    return nullptr;
  }
  if (decl) {
    if (decl->api.allowed_callers == AllowedCallers::AgentOnly) {
      fail(function_errors::caller_disallowed(name), name);
      return nullptr;
    }
    if (decl->api.requires_user_activation && !user_activated_) {
      fail(function_errors::user_activation_required(name), name);
      return nullptr;
    }
  }
  Json args;
  if (!resolve_arguments(call, &args)) return nullptr;
  if (!is_json_representable(args)) {
    fail(make_error(ErrorKind::Validation, "The arguments of function '" + name + "' are not valid JSON.",
                    "EXPRESSION_ERROR"),
         name);
    return nullptr;
  }
  if (decl) {
    args = apply_defaults(decl->api.args_schema, coerce_validation_results(decl->api.args_schema, args));
    if (auto v = host_->validate_function_args(decl->api, args); !v.ok()) {
      fail(function_errors::invalid_arguments(name), name);
      return nullptr;
    }
  }
  Json value;
  switch (host_->agent_function_value(name, catalog_id, args, &value)) {
    case AgentCallStatus::Answered: return value.is_undefined() ? Json(nullptr) : value;
    case AgentCallStatus::Pending: deferred_ = true; return nullptr;
    case AgentCallStatus::Failed: failed_ = true; return nullptr;
  }
  return nullptr;
}

}  // namespace a2ui

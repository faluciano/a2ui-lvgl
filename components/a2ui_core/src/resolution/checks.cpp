// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Check rule evaluation (blueprint features/node_resolution "Checks"; the
// GenericBinder._process_rule_val / _compute_validation_summary of the
// Python core). See evaluate_checks in a2ui/node_resolver.h.

#include <string>
#include <vector>

#include "a2ui/coerce.h"
#include "a2ui/node_resolver.h"

namespace a2ui {
namespace {

std::string str(const Json& j) { return j.is_string() ? j.as_string() : std::string(); }

// Normalizes one rule outcome to a ValidationResult. `explicit_msg`
// overrides the outcome's own message; `fallback` fills a missing one.
// The private `_is_obj` member records whether the outcome was a
// ValidationResult object (it then keeps its severity in validationResult).
Json rule_result(const Json& val, const Json* explicit_msg, const std::string& fallback) {
  bool is_obj = false;
  Json raw;
  if (val.is_object()) {
    is_obj = val.contains("severity") || val.contains("code");
    raw = val;
  } else {
    raw = Json::object({{"valid", Json(coerce_bool(val))}});
  }
  if (explicit_msg) {
    raw.set("message", *explicit_msg);
  } else if (!raw["message"].is_string() || raw["message"].as_string().empty()) {
    raw.set("message", Json(fallback));
  }
  Json dumped = Json::object();
  const std::string sev = str(raw["severity"]);
  const bool well_formed = raw["valid"].is_bool() && (raw["code"].is_undefined() || raw["code"].is_string()) &&
                           raw["message"].is_string() &&
                           (raw["severity"].is_nullish() || sev == "error" || sev == "warning" || sev == "info");
  if (well_formed) {
    dumped.set("valid", raw["valid"]);
    if (raw["code"].is_string()) dumped.set("code", raw["code"]);
    dumped.set("message", raw["message"]);
    if (!sev.empty()) dumped.set("severity", raw["severity"]);
  } else {
    dumped.set("valid", Json(false));
    dumped.set("message", explicit_msg ? *explicit_msg : Json(fallback));
  }
  if (!dumped["valid"].as_bool() && !dumped.contains("severity")) dumped.set("severity", Json("error"));
  dumped.set("_is_obj", Json(is_obj));
  return dumped;
}

}  // namespace

Json evaluate_checks(const Json& rules, DataContext& ctx) {
  std::vector<Json> results;
  for (const auto& rule : rules.as_array()) {
    Json condition = rule;
    const Json* rule_msg = nullptr;
    if (rule.is_object()) {
      if (rule.contains("condition")) condition = rule["condition"];
      if (rule["message"].is_string()) rule_msg = &rule["message"];
    }
    // A path-bound condition reads a stored outcome, whose own message wins;
    // a function's outcome takes the rule's static message.
    const bool path_bound = condition.is_object() && condition["@path"].is_string();
    const Json* explicit_msg = path_bound ? nullptr : rule_msg;
    const std::string fallback = rule_msg ? rule_msg->as_string() : std::string("Validation failed");
    results.push_back(rule_result(ctx.resolve_value(condition), explicit_msg, fallback));
  }
  Json errors{Json::Array{}}, all{Json::Array{}};
  bool valid = true;
  const Json* first = nullptr;
  for (const auto& r : results) {
    if (r["valid"].as_bool()) continue;
    if (!first) first = &r;
    Json clean = r;
    clean.erase("_is_obj");
    all.push_back(std::move(clean));
    if (str(r["severity"]).empty() || str(r["severity"]) == "error") {
      valid = false;
      if (!str(r["message"]).empty()) errors.push_back(r["message"]);
    }
  }
  Json result = Json::object({{"valid", Json(true)}});
  if (first) {
    result = *first;
    result.erase("_is_obj");
    if (!(*first)["_is_obj"].as_bool()) result.erase("severity");
  }
  return Json::object({{"isValid", Json(valid)},
                       {"validationErrors", std::move(errors)},
                       {"validationResults", std::move(all)},
                       {"validationResult", std::move(result)}});
}

}  // namespace a2ui

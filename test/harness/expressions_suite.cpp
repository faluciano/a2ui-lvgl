// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Runner for the `parse_expression_template` cases in core/expressions.yaml.
// Per the harness guide, adjacent literal parts are joined and empty ones
// dropped before comparing, and the suite's `ParseError` maps to
// A2uiExpressionError.

#include "a2ui/expressions.h"
#include "harness.h"

namespace {

using harness::Json;
using harness::Outcome;

Json join_literals(const Json& parts) {
  Json out = Json::array();
  for (const auto& p : parts.as_array()) {
    if (p.is_string()) {
      if (p.as_string().empty()) continue;
      if (out.size() > 0 && out.as_array().back().is_string()) {
        Json& last = out.as_array_mut().back();
        last = Json(last.as_string() + p.as_string());
        continue;
      }
    }
    out.push_back(p);
  }
  return out;
}

Outcome run_parse_expression_template(const Json& tc, const harness::Context&) {
  auto r = a2ui::expressions::parse_template(tc["input"].as_string());
  if (const Json* expect_error = harness::find_expect_error(tc)) {
    if (r.ok()) return harness::missing_error(*expect_error);
    Json expect = *expect_error;
    if (expect["category"].is_string() && expect["category"].as_string() == "ParseError") {
      expect.set("category", Json("ExpressionError"));
    }
    return harness::check_error(r.error(), expect);
  }
  if (!r.ok()) return Outcome::fail("parse failed: " + r.error().message);
  auto m = harness::json_mismatch(join_literals(tc["expect"]), join_literals(r.value()));
  return m.empty() ? Outcome::pass() : Outcome::fail(m);
}

harness::ActionRegistrar parse_expression_template_registrar("parse_expression_template",
                                                             &run_parse_expression_template);

}  // namespace

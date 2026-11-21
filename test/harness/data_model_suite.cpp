// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Runners for core/data_model.yaml (`data_model`) and the `resolve_path`
// cases in core/data_context.yaml and core/message_processor_v0_9.yaml.

#include <memory>
#include <vector>

#include "a2ui/data_model.h"
#include "harness.h"

namespace {

using a2ui::DataModel;
using harness::Json;
using harness::Outcome;

Outcome run_data_model(const Json& tc, const harness::Context&) {
  DataModel model(tc["initial"].is_undefined() ? Json::object() : tc["initial"]);
  std::vector<std::string> notified;
  std::vector<a2ui::Subscription> subs;
  const Json& watch = tc["watch"];
  for (const auto& w : watch.as_array()) {
    const std::string p = w.as_string();
    subs.push_back(model.subscribe(p, [&notified, p](const Json&) { notified.push_back(p); }));
  }

  int step_no = 0;
  for (const auto& step : tc["steps"].as_array()) {
    ++step_no;
    const std::string where = "step " + std::to_string(step_no) + ": ";
    const std::string op = step["op"].as_string();
    const std::string p = step["path"].as_string();
    notified.clear();
    const Json* expect_error = harness::find_expect_error(step);

    if (op == "get") {
      auto r = model.get(p);
      if (expect_error) {
        if (r.ok()) return harness::missing_error(*expect_error);
        auto o = harness::check_error(r.error(), *expect_error);
        if (!o.passed()) return Outcome::fail(where + o.detail);
        continue;
      }
      if (!r.ok()) return Outcome::fail(where + "get failed: " + r.error().message);
      const Json* v = r.value();
      if (step["expect_absent"].as_bool()) {
        if (v) return Outcome::fail(where + "expected absent at " + p + ", got " + v->dump());
      }
      if (const Json* e = step.find("expect")) {
        if (!v) return Outcome::fail(where + "expected " + e->dump() + " at " + p + ", got absent");
        auto m = harness::json_mismatch(*e, *v);
        if (!m.empty()) return Outcome::fail(where + m);
      }
      if (const Json* t = step.find("expect_type")) {
        const bool ok = v && ((t->as_string() == "list" && v->is_array()) ||
                              (t->as_string() == "object" && v->is_object()));
        if (!ok) return Outcome::fail(where + "expected type " + t->as_string() + " at " + p);
      }
      continue;
    }

    a2ui::Status status;
    if (op == "set") {
      status = model.set(p, step["value"]);
    } else if (op == "delete") {
      status = model.remove(p);
    } else if (op == "dispose") {
      model.dispose();
    } else if (op == "mutate_input") {
      // The model stored its own copy, so mutating the caller's value is a no-op here.
    } else {
      return Outcome::unsupported("unknown op " + op);
    }

    if (expect_error) {
      if (status.ok()) return harness::missing_error(*expect_error);
      auto o = harness::check_error(status.error(), *expect_error);
      if (!o.passed()) return Outcome::fail(where + o.detail);
    } else if (!status.ok()) {
      return Outcome::fail(where + op + " failed: " + status.error().message);
    }

    if (const Json* en = step.find("expect_notified")) {
      std::vector<std::string> want;
      for (const auto& e : en->as_array()) want.push_back(e.as_string());
      if (want != notified) {
        Json got = Json::array();
        for (const auto& n : notified) got.push_back(n);
        return Outcome::fail(where + "notified " + got.dump() + ", expected " + en->dump());
      }
    }
    if (const Json* ev = step.find("expect_values")) {
      for (const auto& m : ev->as_object()) {
        auto mm = harness::json_mismatch(m.second, model.value_at(m.first));
        if (!mm.empty()) return Outcome::fail(where + m.first + ": " + mm);
      }
    }
  }
  return Outcome::pass();
}

Outcome run_resolve_path(const Json& tc, const harness::Context&) {
  const Json& args = tc["args"];
  const std::string ctx = args["contextPath"].is_string() ? args["contextPath"].as_string() : "/";
  const std::string got = a2ui::path::resolve(args["path"].as_string(), ctx);
  auto m = harness::json_mismatch(tc["expect"], Json(got));
  return m.empty() ? Outcome::pass() : Outcome::fail(m);
}

harness::ActionRegistrar data_model_registrar("data_model", &run_data_model);
harness::ActionRegistrar resolve_path_registrar("resolve_path", &run_resolve_path);

}  // namespace

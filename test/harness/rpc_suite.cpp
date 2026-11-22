// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Runners for the RPC layer: `handle_rpc` (core/rpc_functions.yaml) and
// `route_agent_function` (core/agent_function_routing.yaml). Modelled on
// python/a2ui_core/tests/test_conformance.py (validate_handle_rpc_case,
// _run_route_agent_function_case) and conformance/README.md "Writing cases
// for `route_agent_function`".
//
// Time is a fake clock: the processor's RpcHandler only advances through
// processor.tick(now_ms), so a timeout fires exactly when a case moves time.
//
// Reactive evaluations (a component property, a binding, a check rule) are
// modelled here without a NodeResolver: each probe evaluates through its own
// AgentCallTracker, records the data paths it read, and re-evaluates when one
// of them changes or when an agent call it made settles
// (SurfaceModel::on_agent_call_settled), as a resolved node does.

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "a2ui/agent_call.h"
#include "a2ui/basic_catalog.h"
#include "a2ui/catalog.h"
#include "a2ui/coerce.h"
#include "a2ui/data_context.h"
#include "a2ui/message_processor.h"
#include "a2ui/rpc.h"
#include "a2ui/spec_documents.h"
#include "a2ui/surface_model.h"
#include "harness.h"

using a2ui::AgentCallTracker;
using a2ui::AllowedCallers;
using a2ui::Catalog;
using a2ui::CatalogPtr;
using a2ui::DataContext;
using a2ui::Error;
using a2ui::FunctionContext;
using a2ui::FunctionDecl;
using a2ui::MessageProcessor;
using a2ui::MessageProcessorOptions;
using a2ui::Result;
using a2ui::Status;
using a2ui::SurfaceModel;
using harness::Json;
using harness::Outcome;

namespace {

const std::string& str(const Json& j) { return j.as_string(); }

/** Decodes the {"$number": "NaN" | "Infinity" | "-Infinity"} encoding of tools/yaml2json.py. */
Json decode_numbers(const Json& v) {
  if (v.is_array()) {
    Json out{Json::Array{}};
    for (const auto& e : v.as_array()) out.push_back(decode_numbers(e));
    return out;
  }
  if (!v.is_object()) return v;
  if (v.size() == 1 && v["$number"].is_string()) {
    const std::string& s = v["$number"].as_string();
    if (s == "NaN") return Json(std::nan(""));
    if (s == "Infinity") return Json(HUGE_VAL);
    if (s == "-Infinity") return Json(-HUGE_VAL);
  }
  Json out = Json::object();
  for (const auto& m : v.as_object()) out.set(m.first, decode_numbers(m.second));
  return out;
}

const CatalogPtr& basic_v1() {
  static CatalogPtr c = [] {
    auto r = Catalog::from_json(*a2ui::spec::parsed(a2ui::spec::Document::BasicCatalogV1));
    CatalogPtr cat = r.ok() ? r.value() : a2ui::make_ref<Catalog>();
    a2ui::basic_catalog::bind_functions(*cat);
    return cat;
  }();
  return c;
}

/** The basic catalog's functions (bound), under another id and version. */
CatalogPtr basic_functions_as(const std::string& id, const std::string& version) {
  static const std::vector<a2ui::DeriveEntry> kNone;
  auto r = basic_v1()->derive(id, &kNone, nullptr);
  CatalogPtr c = r.ok() ? r.value() : a2ui::make_ref<Catalog>();
  c->set_id(id);
  a2ui::basic_catalog::bind_functions(*c);
  c->set_protocol_version(a2ui::normalize_protocol_version(version));
  return c;
}

AllowedCallers parse_callers(const Json& v, AllowedCallers fallback) {
  if (!v.is_string()) return fallback;
  if (str(v) == "agentOnly") return AllowedCallers::AgentOnly;
  if (str(v) == "rendererOrAgent") return AllowedCallers::RendererOrAgent;
  if (str(v) == "rendererOnly") return AllowedCallers::RendererOnly;
  return fallback;
}

std::vector<Json> filter_outbound(const std::vector<Json>& all, const char* kind, size_t from = 0) {
  std::vector<Json> out;
  for (size_t i = from; i < all.size(); ++i) {
    if (all[i].contains(kind)) out.push_back(all[i]);
  }
  return out;
}

bool contains_ci(const std::string& haystack, const std::string& needle) {
  auto lower = [](std::string t) {
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return t;
  };
  return lower(haystack).find(lower(needle)) != std::string::npos;
}

/** Whether a check rule condition passes: a pending rule passes, a failed one does not. */
bool check_passes(DataContext& ctx, const Json& condition) {
  ctx.reset_flags();
  Json v = ctx.resolve_value(condition);
  if (ctx.had_failure()) return false;
  if (ctx.had_deferred()) return true;
  if (v.is_object() && v["valid"].is_bool()) return v["valid"].as_bool();
  return a2ui::coerce_bool(v);
}

/**
 * A reactive evaluation on a surface: re-evaluates when a data path it read
 * changes or an agent call it made settles (flush_probes).
 */
struct Probe {
  Json expr;
  bool is_check = false;
  std::unique_ptr<AgentCallTracker> tracker;
  Json value;
  bool dirty = false;
  std::vector<a2ui::Subscription> subs;
};

void evaluate_probe(Probe& p, SurfaceModel& surface) {
  p.tracker->begin_pass();
  std::vector<std::string> deps;
  DataContext ctx(surface.data_model(), p.tracker.get(), "/");
  ctx.set_dependency_sink(&deps);
  if (p.is_check) {
    p.value = Json(check_passes(ctx, p.expr));
  } else {
    Json v = ctx.resolve_value(p.expr);
    p.value = v.is_undefined() ? Json(nullptr) : std::move(v);
  }
  p.subs.clear();
  std::sort(deps.begin(), deps.end());
  deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
  Probe* raw = &p;
  for (const auto& d : deps) {
    p.subs.push_back(surface.data_model().subscribe(d, [raw](const Json&) { raw->dirty = true; }));
  }
}

struct Probes {
  std::vector<std::unique_ptr<Probe>> list;
  a2ui::Subscription settled;

  void watch(SurfaceModel& surface) {
    settled = surface.on_agent_call_settled().subscribe([this](const a2ui::AgentCallSettlement& s) {
      for (auto& p : list) {
        if (p->tracker->makes_call(s.key)) p->dirty = true;
      }
    });
  }
  Probe& add(SurfaceModel& surface, Json expr, bool is_check) {
    auto p = std::make_unique<Probe>();
    p->expr = std::move(expr);
    p->is_check = is_check;
    p->tracker = std::make_unique<AgentCallTracker>(surface);
    list.push_back(std::move(p));
    evaluate_probe(*list.back(), surface);
    return *list.back();
  }
  /** Re-evaluates dirty probes until none is left (the reference harnesses' "flush async work"). */
  void flush(SurfaceModel& surface) {
    for (int round = 0; round < 50; ++round) {
      bool any = false;
      for (auto& p : list) {
        if (!p->dirty) continue;
        p->dirty = false;
        any = true;
        evaluate_probe(*p, surface);
      }
      if (!any) return;
    }
  }
  /** Drops the trackers and subscriptions (before the surface goes away); keeps the values. */
  void detach() {
    settled.unsubscribe();
    for (auto& p : list) {
      p->subs.clear();
      p->tracker.reset();
    }
  }
};

// ---------------------------------------------------------------------------
// handle_rpc

FunctionDecl mock_function(const std::string& name, const Json& meta) {
  FunctionDecl d;
  d.api.name = name;
  d.api.return_type = meta["returnType"].is_string() ? str(meta["returnType"]) : "any";
  if (meta["schema"].is_object()) {
    d.api.args_schema = meta["schema"];
  } else if (meta["parameters"].is_object()) {
    d.api.args_schema = meta["parameters"];
  } else {
    d.api.args_schema = Json::object({{"type", Json("object")}});
  }
  d.api.allowed_callers = parse_callers(meta["allowedCallers"], AllowedCallers::RendererOrAgent);
  d.api.requires_user_activation = meta["requiresUserActivation"].as_bool(false);
  d.impl = [name](const Json& args, FunctionContext&) -> Result<Json> {
    if (name == "playMedia") return Json::object({{"playing", Json(true)}, {"timestamp", Json(0)}});
    if (name == "openExternalUrl") return Json::object({{"opened", Json(true)}});
    if (name == "syncState") return Json(nullptr);
    if (name == "failingFunction") {
      return a2ui::make_error(a2ui::ErrorKind::Generic, "An error occurred during function execution.");
    }
    if (name == "calculateTax") return Json(args["amount"].as_number() * 0.1);
    if (name == "echoArgs") return args;
    return Json(nullptr);
  };
  return d;
}

/** A scenario's localFunctions: {returnType, effect: {setData: {path, value}}}. */
void add_scenario_functions(Catalog& cat, const Json& spec) {
  for (const auto& m : spec.as_object()) {
    FunctionDecl d;
    d.api.name = m.first;
    d.api.return_type = m.second["returnType"].is_string() ? str(m.second["returnType"]) : "any";
    d.api.args_schema = Json::object({{"type", Json("object")}});
    const Json set_data = m.second["effect"]["setData"];
    d.impl = [set_data](const Json&, FunctionContext& ctx) -> Result<Json> {
      // DataContext is the only FunctionContext implementation.
      if (set_data["path"].is_string()) (void)static_cast<DataContext&>(ctx).set(str(set_data["path"]), set_data["value"]);
      return Json(nullptr);
    };
    cat.add_function(std::move(d));
  }
}

/** The catalog id and version of a case (Python _resolve_rpc_catalog_info). */
void rpc_catalog_info(const Json& tc, std::string* id, std::string* version) {
  const Json& args = tc["args"];
  *id = args["catalogId"].is_string() ? str(args["catalogId"]) : "";
  const Json& msg = args["message"];
  if (id->empty() && msg.contains("callRendererFunction")) {
    const Json& cat = msg["callRendererFunction"]["callFunction"]["catalogId"];
    const Json& err_msg = tc["expect"]["response"]["rendererFunctionResponse"]["error"]["message"];
    const bool not_found = err_msg.is_string() && str(err_msg).find("Catalog not found") != std::string::npos;
    if (!not_found && cat.is_string()) *id = str(cat);
  }
  if (id->empty() && args["outboundCall"]["callFunction"]["catalogId"].is_string()) {
    *id = str(args["outboundCall"]["callFunction"]["catalogId"]);
  }
  if (id->empty()) *id = "basic";
  if (args["catalogVersion"].is_string()) {
    *version = str(args["catalogVersion"]);
  } else if (tc["catalog"]["protocolVersion"].is_string()) {
    *version = str(tc["catalog"]["protocolVersion"]);
  } else {
    *version = "1.0";
  }
}

std::vector<CatalogPtr> rpc_catalogs(const std::string& id, const std::string& version, const Json& fn_meta) {
  CatalogPtr cat;
  if (id == "basic") {
    cat = basic_functions_as("basic", version);
  } else {
    cat = a2ui::make_ref<Catalog>();
    cat->set_id(id);
    cat->set_protocol_version(a2ui::normalize_protocol_version(version));
  }
  for (const auto& m : fn_meta.as_object()) cat->add_function(mock_function(m.first, m.second));
  std::vector<CatalogPtr> out{cat};
  if (id != "basic") out.push_back(basic_functions_as("basic", version));
  return out;
}

struct CallOutcome {
  int count = 0;
  bool ok = false;
  Json value;
  Error error;
};

struct RpcFixture {
  // Declared first so they outlive the processor, whose teardown cancels pending calls.
  std::vector<std::unique_ptr<CallOutcome>> outcomes;
  std::vector<Json> outbound;
  std::vector<Json> processor_errors;
  std::unique_ptr<MessageProcessor> processor;
  a2ui::Subscription error_sub;
  uint64_t now = 1000;

  RpcFixture(std::vector<CatalogPtr> catalogs, bool listener) {
    MessageProcessorOptions opts;
    if (listener) opts.outbound_listener = [this](const Json& m) { outbound.push_back(m); };
    processor = std::make_unique<MessageProcessor>(std::move(catalogs), std::move(opts));
    error_sub = processor->on_error().subscribe([this](const Json& e) { processor_errors.push_back(e); });
    processor->tick(now);
  }
  CallOutcome& new_outcome() {
    outcomes.push_back(std::make_unique<CallOutcome>());
    return *outcomes.back();
  }
};

Result<std::string> start_outbound(RpcFixture& f, const Json& spec, CallOutcome* out) {
  Json call = spec["callFunction"];
  if (!call["catalogId"].is_string()) call.set("catalogId", Json(a2ui::basic_catalog::kCatalogIdV1));
  a2ui::AgentCallOptions opts;
  opts.function_call_id = spec["functionCallId"].is_string() ? str(spec["functionCallId"]) : "";
  if (spec["timeoutMs"].is_number()) opts.timeout_ms = static_cast<uint32_t>(spec["timeoutMs"].as_number());
  return f.processor->call_agent_function(str(spec["surfaceId"]), call, [out](Result<Json> r) {
    ++out->count;
    out->ok = r.ok();
    if (r.ok()) {
      out->value = std::move(r).value();
    } else {
      out->error = r.take_error();
    }
  }, opts);
}

Outcome expect_code(const Error& e, const Json& expected, const char* fallback, const char* what) {
  const std::string want = expected["code"].is_string() ? str(expected["code"]) : fallback;
  if (e.code != want) return Outcome::fail(std::string(what) + ": expected " + want + ", got " + e.code + " (" + e.message + ")");
  return Outcome::pass();
}

Outcome check_late_response(RpcFixture& f, const Json& args, const Json& expect, const CallOutcome& first) {
  const Json& late = args["lateInboundResponse"];
  if (!late.is_object()) return Outcome::pass();
  const size_t out_before = f.outbound.size(), err_before = f.processor_errors.size();
  const int count_before = first.count;
  const bool ok_before = first.ok;
  const std::string code_before = first.error.code;
  Status s = f.processor->process_messages(late);
  if (expect["lateResponseIgnored"].as_bool(false)) {
    if (!s.ok()) return Outcome::fail("late response raised: " + s.error().message);
    if (f.outbound.size() != out_before) return Outcome::fail("late response sent a message");
    if (f.processor_errors.size() != err_before) return Outcome::fail("late response reported an error");
    if (f.processor->rpc().pending_call_count() != 0) return Outcome::fail("pending calls left after late response");
    if (first.count != count_before || first.ok != ok_before || first.error.code != code_before) {
      return Outcome::fail("late response changed the settled outcome");
    }
  }
  return Outcome::pass();
}

Outcome run_outbound_error(RpcFixture& f, const Json& args, const Json& expected_err, const Json& expect) {
  const Json& call = args["outboundCall"];
  CallOutcome& first = f.new_outcome();
  if (args["outboundListener"].is_bool() && !args["outboundListener"].as_bool()) {
    auto r = start_outbound(f, call, &first);
    if (r.ok()) return Outcome::fail("call without a listener succeeded");
    return expect_code(r.error(), expected_err, "NO_LISTENER", "no listener");
  }
  if (args.contains("secondOutboundCall")) {
    auto r1 = start_outbound(f, call, &first);
    if (!r1.ok()) return Outcome::fail("first call failed: " + r1.error().message);
    CallOutcome& second = f.new_outcome();
    auto r2 = start_outbound(f, args["secondOutboundCall"], &second);
    if (r2.ok()) return Outcome::fail("duplicate call was accepted");
    if (second.count) return Outcome::fail("rejected call ran its callback");
    return expect_code(r2.error(), expected_err, "DUPLICATE", "duplicate");
  }
  if (args["inboundResponse"].is_object()) {
    auto r = start_outbound(f, call, &first);
    if (!r.ok()) return Outcome::fail("call failed: " + r.error().message);
    (void)f.processor->process_messages(args["inboundResponse"]);
    if (first.count != 1 || first.ok) return Outcome::fail("call did not fail");
    if (expected_err["message"].is_string() && !contains_ci(first.error.message, str(expected_err["message"]))) {
      return Outcome::fail("error message: " + first.error.message);
    }
    return expect_code(first.error, expected_err, first.error.code.c_str(), "agent error");
  }
  const std::string after = args["afterCall"].is_string() ? str(args["afterCall"]) : "";
  if (after == "cancel") {
    auto r = start_outbound(f, call, &first);
    if (!r.ok()) return Outcome::fail("call failed: " + r.error().message);
    f.processor->rpc().cancel_surface_calls(str(call["surfaceId"]));
    if (first.count != 1 || first.ok) return Outcome::fail("call was not cancelled");
    return expect_code(first.error, expected_err, "CANCELLED", "cancel");
  }
  if (after == "dispose") {
    auto r = start_outbound(f, call, &first);
    if (!r.ok()) return Outcome::fail("call failed: " + r.error().message);
    f.processor->rpc().dispose();
    if (first.count != 1 || first.ok) return Outcome::fail("in-flight call was not rejected");
    Outcome o = expect_code(first.error, expect["inFlightError"], "CANCELLED", "in-flight");
    if (!o.passed()) return o;
    CallOutcome& again = f.new_outcome();
    auto r2 = start_outbound(f, call, &again);
    if (r2.ok()) return Outcome::fail("call after dispose was accepted");
    o = expect_code(r2.error(), expected_err, "DISPOSED", "after dispose");
    if (!o.passed()) return o;
    return check_late_response(f, args, expect, first);
  }
  if (call["timeoutMs"].is_number()) {
    auto r = start_outbound(f, call, &first);
    if (!r.ok()) return Outcome::fail("call failed: " + r.error().message);
    const uint64_t timeout = static_cast<uint64_t>(call["timeoutMs"].as_number());
    if (timeout > 0) {
      f.processor->tick(f.now + timeout - 1);
      if (first.count) return Outcome::fail("call settled before its timeout");
    }
    f.now += timeout;
    f.processor->tick(f.now);
    if (first.count != 1 || first.ok) return Outcome::fail("call did not time out");
    Outcome o = expect_code(first.error, expected_err, "TIMEOUT", "timeout");
    if (!o.passed()) return o;
    if (f.processor->rpc().pending_call_count()) return Outcome::fail("timed-out call still pending");
    return check_late_response(f, args, expect, first);
  }
  return Outcome::fail("unhandled outboundCall error scenario");
}

Outcome run_inbound(RpcFixture& f, const Json& tc, bool user_activation) {
  const Json& message = tc["args"]["message"];
  const Json& expect = tc["expect"];
  const Json* expect_error = harness::find_expect_error(tc);
  const Json& expect_err_obj = expect["error"];
  a2ui::ExecutionContext ec;
  ec.is_user_activated = user_activation;
  const size_t out_before = f.outbound.size();
  Status st = f.processor->process_messages(message, ec);
  if (expect_error || expect_err_obj.is_object()) {
    if (st.ok()) return Outcome::fail("expected an error, got success");
    const Error& e = st.error();
    if (expect_err_obj.is_object()) {
      if (expect_err_obj["code"].is_string() && e.code != str(expect_err_obj["code"])) {
        return Outcome::fail("error code " + e.code + " (" + e.message + ")");
      }
      if (expect_err_obj["functionCallId"].is_string() && e.function_call_id != str(expect_err_obj["functionCallId"])) {
        return Outcome::fail("error functionCallId " + e.function_call_id);
      }
      if (expect_err_obj["message"].is_string() && !contains_ci(e.message, str(expect_err_obj["message"]))) {
        return Outcome::fail("error message: " + e.message);
      }
    }
    if (expect_error) {
      Json filter = Json::object();
      for (const char* k : {"category", "functionCallId"}) {
        if (const Json* v = expect_error->find(k)) filter.set(k, *v);
      }
      Outcome o = harness::check_error(e, filter);
      if (!o.passed()) return o;
      if (const Json* re = expect_error->find("rendererError"); re && re->is_object()) {
        auto envelopes = filter_outbound(f.outbound, "error", out_before);
        if (envelopes.size() != 1) return Outcome::fail("expected one error envelope, got " + std::to_string(envelopes.size()));
        const Json& got = envelopes[0];
        if (got["version"] != (*re)["version"]) return Outcome::fail("error envelope version: " + got.dump());
        const Json& want_body = (*re)["error"];
        const Json& got_body = got["error"];
        for (const char* k : {"code", "functionCallId"}) {
          if (want_body.contains(k) && got_body[k] != want_body[k]) return Outcome::fail("error envelope: " + got.dump());
        }
        if (want_body["message"].is_string() && !contains_ci(str(got_body["message"]), str(want_body["message"]))) {
          return Outcome::fail("error envelope message: " + got.dump());
        }
      }
    }
    return Outcome::pass();
  }
  if (!expect.contains("response")) return Outcome::fail("inbound case needs expectError, expect.error or expect.response");
  if (!st.ok()) return Outcome::fail("processing failed: " + st.error().message);
  auto responses = filter_outbound(f.outbound, "rendererFunctionResponse", out_before);
  const Json& want = expect["response"];
  if (want.is_null()) {
    if (!responses.empty()) return Outcome::fail("unexpected response " + responses[0].dump());
    if (f.outbound.size() != out_before) return Outcome::fail("ignored message sent " + f.outbound.back().dump());
    if (!f.processor_errors.empty()) return Outcome::fail("ignored message reported " + f.processor_errors[0].dump());
    if (f.processor->rpc().pending_call_count()) return Outcome::fail("pending calls left");
    return Outcome::pass();
  }
  if (responses.size() != 1) return Outcome::fail("expected one response, got " + std::to_string(responses.size()));
  const Json& got = responses[0];
  if (got["version"] != want["version"]) return Outcome::fail("response version: " + got.dump());
  const Json& wb = want["rendererFunctionResponse"];
  const Json& gb = got["rendererFunctionResponse"];
  if (gb["functionCallId"] != wb["functionCallId"]) return Outcome::fail("response functionCallId: " + got.dump());
  if (wb.contains("value")) {
    if (!gb.contains("value")) return Outcome::fail("response has no value: " + got.dump());
    const std::string mm = harness::json_mismatch(wb["value"], gb["value"]);
    if (!mm.empty()) return Outcome::fail("response value: " + mm + " in " + got.dump());
  }
  if (wb.contains("error")) {
    const Json& ge = gb["error"];
    if (ge["code"] != wb["error"]["code"]) return Outcome::fail("response error: " + got.dump());
    if (wb["error"]["message"].is_string() && !contains_ci(str(ge["message"]), str(wb["error"]["message"]))) {
      return Outcome::fail("response error message: " + got.dump());
    }
  }
  return Outcome::pass();
}

/** Checks the single callAgentFunction a scenario sent and returns its functionCallId. */
Outcome check_scenario_call(const RpcFixture& f, const Json& expected, std::string* call_id) {
  auto calls = filter_outbound(f.outbound, "callAgentFunction");
  if (calls.size() != 1) return Outcome::fail("expected one callAgentFunction, got " + std::to_string(calls.size()));
  const Json& got = calls[0];
  if (got["version"] != expected["version"]) return Outcome::fail("outbound version: " + got.dump());
  const Json& gb = got["callAgentFunction"];
  const Json& wb = expected["callAgentFunction"];
  if (gb["surfaceId"] != wb["surfaceId"]) return Outcome::fail("outbound surfaceId: " + got.dump());
  const std::string mm = harness::json_mismatch(wb["callFunction"], gb["callFunction"]);
  if (!mm.empty() || gb["callFunction"].size() != wb["callFunction"].size()) {
    return Outcome::fail("outbound callFunction: " + mm + " in " + got.dump());
  }
  if (!gb["functionCallId"].is_string() || str(gb["functionCallId"]).empty()) return Outcome::fail("no functionCallId");
  *call_id = str(gb["functionCallId"]);
  return Outcome::pass();
}

Outcome respond_to_scenario(RpcFixture& f, const Json& scenario, const std::string& call_id) {
  Json body = Json::object({{"functionCallId", Json(call_id)}});
  if (scenario.contains("inboundResponseError")) {
    body.set("error", scenario["inboundResponseError"]);
  } else {
    body.set("value", scenario["inboundResponseValue"].is_undefined() ? Json(nullptr) : scenario["inboundResponseValue"]);
  }
  Status s = f.processor->process_messages(
      Json::object({{"version", Json("1.0")}, {"agentFunctionResponse", std::move(body)}}));
  return s.ok() ? Outcome::pass() : Outcome::fail("response failed: " + s.error().message);
}

Outcome check_surface_error(const std::vector<Json>& errors, const Json& expected, const std::string& call_id) {
  if (!expected.is_object()) return Outcome::pass();
  if (errors.empty()) return Outcome::fail("expected a surface error, got none");
  const Json& got = errors[0];
  if (expected["code"].is_string() && got["code"] != expected["code"]) return Outcome::fail("surface error " + got.dump());
  if (expected["functionCallId"].is_string()) {
    const std::string want = str(expected["functionCallId"]) == "$outboundCallId" ? call_id : str(expected["functionCallId"]);
    if (got["functionCallId"] != Json(want)) return Outcome::fail("surface error functionCallId: " + got.dump());
  }
  return Outcome::pass();
}

SurfaceModel* create_scenario_surface(RpcFixture& f, const Json& scenario, std::vector<Json>* errors,
                                      a2ui::Subscription* sub) {
  Status s = f.processor->process_messages(Json::object(
      {{"version", Json("1.0")},
       {"createSurface",
        Json::object({{"surfaceId", scenario["surfaceId"]}, {"catalogId", scenario["catalogId"]}})}}));
  if (!s.ok()) return nullptr;
  SurfaceModel* surface = f.processor->model().get_surface(str(scenario["surfaceId"]));
  if (surface) *sub = surface->on_error().subscribe([errors](const Json& e) { errors->push_back(e); });
  return surface;
}

Outcome run_ui_binding(RpcFixture& f, const Json& scenario, const Json& expect) {
  std::vector<Json> errors;
  a2ui::Subscription error_sub;
  SurfaceModel* surface = create_scenario_surface(f, scenario, &errors, &error_sub);
  if (!surface) return Outcome::fail("createSurface failed");
  std::vector<Json> comps;
  if (scenario["components"].is_array()) {
    comps = scenario["components"].as_array();
  } else if (scenario["component"].is_object()) {
    comps.push_back(scenario["component"]);
  }
  auto find_comp = [&](const std::string& id) -> const Json* {
    for (const auto& c : comps) {
      if (c["id"] == Json(id)) return &c;
    }
    return nullptr;
  };
  const Json& observe = scenario["observe"];
  const std::string obs_id = observe["componentId"].is_string() ? str(observe["componentId"]) : "root";
  const std::string obs_prop = observe["property"].is_string() ? str(observe["property"]) : "text";
  const Json* obs = find_comp(obs_id);
  if (!obs) return Outcome::fail("observed component not found");

  // The observed property and each check rule are reactive evaluations.
  Probes probes;
  probes.watch(*surface);
  Probe* prop = (*obs).contains(obs_prop) ? &probes.add(*surface, (*obs)[obs_prop], false) : nullptr;
  std::vector<Probe*> checks;
  for (const auto& rule : (*obs)["checks"].as_array()) checks.push_back(&probes.add(*surface, rule["condition"], true));
  probes.flush(*surface);

  if (expect.contains("initialValue")) {
    const std::string mm = harness::json_mismatch(expect["initialValue"], prop ? prop->value : Json(nullptr));
    if (!mm.empty() || (expect["initialValue"].is_null() && prop && !prop->value.is_null())) {
      return Outcome::fail("initial value: " + (prop ? prop->value.dump() : std::string("none")));
    }
  }
  std::string call_id;
  Outcome o = check_scenario_call(f, expect["outboundCall"], &call_id);
  if (!o.passed()) return o;
  o = respond_to_scenario(f, scenario, call_id);
  if (!o.passed()) return o;
  probes.flush(*surface);

  if (expect.contains("resolvedValue")) {
    const Json got = prop ? prop->value : Json(nullptr);
    const std::string mm = harness::json_mismatch(expect["resolvedValue"], got);
    if (!mm.empty() || (expect["resolvedValue"].is_null() && !got.is_null())) {
      return Outcome::fail("resolved value: " + got.dump());
    }
  }
  if (expect["siblings"].is_object()) {
    for (const auto& sib : expect["siblings"].as_object()) {
      const Json* c = find_comp(sib.first);
      if (!c) return Outcome::fail("sibling not found: " + sib.first);
      for (const auto& p : sib.second.as_object()) {
        DataContext ctx = surface->data_context("/");
        const std::string mm = harness::json_mismatch(p.second, ctx.resolve_value((*c)[p.first]));
        if (!mm.empty()) return Outcome::fail("sibling " + sib.first + "." + p.first + ": " + mm);
      }
    }
  }
  if (expect["validationResult"].is_object()) {
    const Json& vr = expect["validationResult"];
    bool valid = true;
    std::string message;
    const auto& rules = (*obs)["checks"].as_array();
    for (size_t i = 0; i < checks.size(); ++i) {
      if (!checks[i]->value.as_bool()) {
        if (valid) message = rules[i]["message"].is_string() ? str(rules[i]["message"]) : "";
        valid = false;
      }
    }
    if (vr["valid"].is_bool() && vr["valid"].as_bool() != valid) return Outcome::fail("validation result valid mismatch");
    if (vr["message"].is_string() && str(vr["message"]) != message) return Outcome::fail("validation message: " + message);
    if (vr["severity"].is_string() && (valid || str(vr["severity"]) != "error")) {
      return Outcome::fail("validation severity mismatch");
    }
  }
  o = check_surface_error(errors, expect["surfaceError"], call_id);
  probes.detach();
  return o;
}

Outcome run_ui_action(RpcFixture& f, const Json& scenario, const Json& expect) {
  std::vector<Json> errors;
  a2ui::Subscription error_sub;
  SurfaceModel* surface = create_scenario_surface(f, scenario, &errors, &error_sub);
  if (!surface) return Outcome::fail("createSurface failed");
  int done_count = 0;
  Json result;
  surface->run_action(scenario["actionPayload"], "", "/", true, [&](const Json& r) {
    ++done_count;
    result = r;
  });
  std::string call_id;
  Outcome o = check_scenario_call(f, expect["outboundCall"], &call_id);
  if (!o.passed()) return o;
  o = respond_to_scenario(f, scenario, call_id);
  if (!o.passed()) return o;
  if (expect.contains("result")) {
    if (done_count != 1) return Outcome::fail("action did not complete");
    const std::string mm = harness::json_mismatch(expect["result"], result);
    if (!mm.empty()) return Outcome::fail("action result: " + mm);
  }
  if (expect.contains("dataModel")) {
    const std::string mm = harness::json_mismatch(expect["dataModel"], surface->data_model().root());
    if (!mm.empty() || surface->data_model().root().size() != expect["dataModel"].size()) {
      return Outcome::fail("data model: " + surface->data_model().root().dump());
    }
  }
  return check_surface_error(errors, expect["surfaceError"], call_id);
}

HARNESS_ACTION("handle_rpc", run_handle_rpc) {
  if (harness::targets_pre_v1(test_case)) return Outcome::unsupported("pre-v1.0 case");
  const Json& args = test_case["args"];
  const Json& expect = test_case["expect"];

  std::string cat_id, cat_version;
  rpc_catalog_info(test_case, &cat_id, &cat_version);
  std::vector<CatalogPtr> catalogs = rpc_catalogs(cat_id, cat_version, args["functionMetadata"]);
  for (const char* key : {"uiBindingScenario", "uiActionFallbackScenario"}) {
    const Json& sc = args[key];
    if (!sc.is_object()) continue;
    auto scat = a2ui::make_ref<Catalog>();
    scat->set_id(str(sc["catalogId"]));
    scat->set_protocol_version("1.0");
    add_scenario_functions(*scat, sc["localFunctions"]);
    catalogs.push_back(scat);
  }
  const bool listener = !(args["outboundListener"].is_bool() && !args["outboundListener"].as_bool());
  RpcFixture f(catalogs, listener);

  if (args["surfaceId"].is_string()) {
    a2ui::CatalogMap available;
    for (const auto& c : catalogs) available.emplace(c->id(), c);
    a2ui::SurfaceOptions so;
    so.protocol_version = cat_version;
    if (args["dataModel"].is_object()) so.data_model = args["dataModel"];
    Status s = f.processor->model().add_surface(
        std::make_unique<SurfaceModel>(str(args["surfaceId"]), catalogs[0], std::move(available), std::move(so)));
    if (!s.ok()) return Outcome::fail("surface setup: " + s.error().message);
  }

  bool handled = false;
  if (args["message"].is_object()) {
    Outcome o = run_inbound(f, test_case, args["userActivationPresent"].as_bool(false));
    if (!o.passed()) return o;
    handled = true;
  }

  const Json* expect_error = harness::find_expect_error(test_case);
  const bool has_err = expect_error || expect["error"].is_object();
  if (args["outboundCall"].is_object() && args["inboundResponse"].is_object() && !has_err) {
    const Json& inbound = args["inboundResponse"];
    if (expect["correlatedCallId"].is_string() &&
        inbound["agentFunctionResponse"]["functionCallId"] != expect["correlatedCallId"]) {
      return Outcome::fail("correlatedCallId does not match the response");
    }
    CallOutcome& out = f.new_outcome();
    auto r = start_outbound(f, args["outboundCall"], &out);
    if (!r.ok()) return Outcome::fail("call failed: " + r.error().message);
    if (r.value() != str(args["outboundCall"]["functionCallId"])) return Outcome::fail("functionCallId not kept");
    auto sent = filter_outbound(f.outbound, "callAgentFunction");
    if (sent.size() != 1 || sent[0]["callAgentFunction"]["functionCallId"] != Json(r.value())) {
      return Outcome::fail("callAgentFunction not sent");
    }
    Status s = f.processor->process_messages(inbound);
    if (!s.ok()) return Outcome::fail("response failed: " + s.error().message);
    if (out.count != 1 || !out.ok) return Outcome::fail("call did not resolve");
    const std::string mm = harness::json_mismatch(expect["result"], out.value);
    if (!mm.empty()) return Outcome::fail("result: " + mm);
    handled = true;
  } else if (args["outboundCall"].is_object() && has_err) {
    const Json& expected_err = expect_error ? *expect_error : expect["error"];
    Outcome o = run_outbound_error(f, args, expected_err, expect);
    if (!o.passed()) return o;
    handled = true;
  }

  if (args["uiBindingScenario"].is_object()) {
    Outcome o = run_ui_binding(f, args["uiBindingScenario"], expect);
    if (!o.passed()) return o;
    handled = true;
  }
  if (args["uiActionFallbackScenario"].is_object()) {
    Outcome o = run_ui_action(f, args["uiActionFallbackScenario"], expect);
    if (!o.passed()) return o;
    handled = true;
  }
  return handled ? Outcome::pass() : Outcome::fail("no handle_rpc scenario matched");
}

// ---------------------------------------------------------------------------
// route_agent_function

std::vector<CatalogPtr> routing_catalogs(const Json& specs) {
  std::vector<CatalogPtr> out;
  for (const auto& spec : specs.as_array()) {
    auto cat = a2ui::make_ref<Catalog>();
    cat->set_id(str(spec["catalogId"]));
    cat->set_protocol_version("1.0");
    for (const auto& name : spec["localFunctions"].as_array()) {
      if (const FunctionDecl* d = basic_v1()->find_function(str(name))) cat->add_function(*d);
    }
    for (const auto& m : spec["agentFunctions"].as_object()) {
      FunctionDecl d;  // schema-only: no implementation
      d.api.name = m.first;
      d.api.args_schema = m.second["args"];
      d.api.return_type = m.second["returnType"].is_string() ? str(m.second["returnType"]) : "any";
      d.api.requires_user_activation = m.second["requiresUserActivation"].as_bool(false);
      d.api.allowed_callers = parse_callers(m.second["allowedCallers"], AllowedCallers::RendererOnly);
      cat->add_function(std::move(d));
    }
    out.push_back(cat);
  }
  return out;
}

HARNESS_ACTION("route_agent_function", run_route_agent_function) {
  if (harness::targets_pre_v1(test_case)) return Outcome::unsupported("pre-v1.0 case");
  const Json& surface_spec = test_case["surface"];
  const bool listener = !(surface_spec["outboundListener"].is_bool() && !surface_spec["outboundListener"].as_bool());
  RpcFixture f(routing_catalogs(test_case["catalogs"]), listener);

  Json create = Json::object({{"surfaceId", Json("main")}});
  if (surface_spec["catalogId"].is_string()) create.set("catalogId", surface_spec["catalogId"]);
  Status s = f.processor->process_messages(Json::object({{"version", Json("1.0")}, {"createSurface", create}}));
  if (!s.ok()) return Outcome::fail("createSurface: " + s.error().message);
  SurfaceModel* surface = f.processor->model().get_surface("main");
  if (!surface) return Outcome::fail("no surface");
  if (test_case["dataModel"].is_object()) (void)surface->data_model().set("/", test_case["dataModel"]);

  std::vector<Json> errors, actions;
  a2ui::Subscription err_sub = surface->on_error().subscribe([&](const Json& e) { errors.push_back(e); });
  a2ui::Subscription act_sub = surface->on_action().subscribe([&](const Json& a) { actions.push_back(a); });
  Probes probes;
  probes.watch(*surface);
  size_t outbound_seen = 0, errors_seen = 0, actions_seen = 0;
  bool deleted = false;

  const auto& steps = test_case["steps"].as_array();
  for (size_t i = 0; i < steps.size(); ++i) {
    const Json& step = steps[i];
    const std::string label = "step " + std::to_string(i);
    if (step.contains("evaluate")) {
      if (deleted) return Outcome::fail(label + ": evaluate after deleteSurface");
      const Json expr = decode_numbers(step["evaluate"]);
      const std::string as = step["as"].is_string() ? str(step["as"]) : "binding";
      if (as == "action") {
        surface->run_action(Json::object({{"functionCall", expr}}), "probe");
      } else if (as == "event") {
        surface->run_action(Json::object({{"event", expr}}), "probe");
      } else {
        probes.add(*surface, expr, as == "check");
      }
    } else if (step.contains("respond")) {
      Json body = step["respond"];
      const size_t idx = static_cast<size_t>(body["call"].as_number());
      body.erase("call");
      auto calls = filter_outbound(f.outbound, "callAgentFunction");
      if (idx >= calls.size()) return Outcome::fail(label + ": no call " + std::to_string(idx));
      Json resp = Json::object({{"functionCallId", calls[idx]["callAgentFunction"]["functionCallId"]}});
      for (const auto& m : body.as_object()) resp.set(m.first, m.second);
      Status r = f.processor->process_messages(
          Json::object({{"version", Json("1.0")}, {"agentFunctionResponse", std::move(resp)}}));
      if (!r.ok()) return Outcome::fail(label + ": response failed: " + r.error().message);
    } else if (step.contains("updateData")) {
      if (deleted) return Outcome::fail(label + ": updateData after deleteSurface");
      (void)surface->data_model().set(str(step["updateData"]["path"]), step["updateData"]["value"]);
    } else if (step["cancelSurfaceCalls"].as_bool(false)) {
      f.processor->rpc().cancel_surface_calls("main");
    } else if (step["deleteSurface"].as_bool(false)) {
      probes.detach();
      Status r = f.processor->process_messages(
          Json::object({{"version", Json("1.0")}, {"deleteSurface", Json::object({{"surfaceId", Json("main")}})}}));
      if (!r.ok()) return Outcome::fail(label + ": deleteSurface failed: " + r.error().message);
      deleted = true;
      surface = nullptr;
    } else {
      return Outcome::fail(label + ": unknown step");
    }
    if (surface) probes.flush(*surface);

    // Outbound: only callAgentFunction for surface main, compared by callFunction.
    std::vector<Json> sent;
    for (size_t k = outbound_seen; k < f.outbound.size(); ++k) {
      const Json& m = f.outbound[k];
      if (!m.contains("callAgentFunction")) return Outcome::fail(label + ": unexpected outbound " + m.dump());
      if (m["callAgentFunction"]["surfaceId"] != Json("main")) return Outcome::fail(label + ": wrong surface");
      sent.push_back(m["callAgentFunction"]["callFunction"]);
    }
    outbound_seen = f.outbound.size();
    std::vector<Json> want = step["expectOutbound"].as_array();
    if (step["outboundInAnyOrder"].as_bool(false)) {
      auto by_canonical = [](const Json& a, const Json& b) { return a2ui::canonical_json(a) < a2ui::canonical_json(b); };
      std::sort(sent.begin(), sent.end(), by_canonical);
      std::sort(want.begin(), want.end(), by_canonical);
    }
    if (sent.size() != want.size()) {
      std::string got;
      for (const auto& x : sent) got += x.dump() + " ";
      return Outcome::fail(label + ": sent " + std::to_string(sent.size()) + " calls, expected " +
                           std::to_string(want.size()) + ": " + got);
    }
    for (size_t k = 0; k < sent.size(); ++k) {
      if (a2ui::canonical_json(sent[k]) != a2ui::canonical_json(want[k])) {
        return Outcome::fail(label + ": sent " + sent[k].dump() + ", expected " + want[k].dump());
      }
    }

    const auto& want_errors = step["expectErrors"].as_array();
    const size_t n_err = errors.size() - errors_seen;
    if (n_err != want_errors.size()) {
      std::string got;
      for (size_t k = errors_seen; k < errors.size(); ++k) got += errors[k].dump() + " ";
      return Outcome::fail(label + ": reported " + std::to_string(n_err) + " errors, expected " +
                           std::to_string(want_errors.size()) + ": " + got);
    }
    for (size_t k = 0; k < n_err; ++k) {
      const Json& got = errors[errors_seen + k];
      const Json& w = want_errors[k];
      if (got["code"] != w["code"]) return Outcome::fail(label + ": error " + got.dump());
      if (w["message"].is_string() && str(got["message"]).find(str(w["message"])) == std::string::npos) {
        return Outcome::fail(label + ": error message " + got.dump());
      }
    }
    errors_seen = errors.size();

    Json dispatched{Json::Array{}};
    for (size_t k = actions_seen; k < actions.size(); ++k) {
      Json a = Json::object();
      for (const char* key : {"name", "context", "userMessage"}) {
        if (actions[k].contains(key)) a.set(key, actions[k][key]);
      }
      dispatched.push_back(std::move(a));
    }
    actions_seen = actions.size();
    const Json want_actions = step.contains("expectActions") ? step["expectActions"] : Json(Json::Array{});
    if (a2ui::canonical_json(dispatched) != a2ui::canonical_json(want_actions)) {
      return Outcome::fail(label + ": dispatched " + dispatched.dump() + ", expected " + want_actions.dump());
    }

    if (step.contains("expectValues")) {
      Json values{Json::Array{}};
      for (const auto& p : probes.list) values.push_back(p->value);
      if (a2ui::canonical_json(values) != a2ui::canonical_json(step["expectValues"])) {
        return Outcome::fail(label + ": values " + values.dump() + ", expected " + step["expectValues"].dump());
      }
    }
  }
  probes.detach();
  return Outcome::pass();
}

}  // namespace

// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// RPC layer behaviour the conformance suites do not cover: timeouts on the
// injectable clock (route_agent_function uses no timers), cancellation and
// disposal of reactive and action calls, re-entrant listeners, and the
// bounded agent call cache.

#include <memory>
#include <string>
#include <vector>

#include "a2ui/agent_call.h"
#include "a2ui/catalog.h"
#include "a2ui/data_context.h"
#include "a2ui/message_processor.h"
#include "a2ui/rpc.h"
#include "a2ui/surface_model.h"
#include "test_main.h"

using a2ui::AgentCallOptions;
using a2ui::Json;
using a2ui::MessageProcessor;
using a2ui::Result;
using a2ui::RpcHandler;

namespace {

Json parse(const char* text) { return Json::parse(text).value; }

struct Settled {
  int count = 0;
  bool ok = false;
  Json value;
  a2ui::Error error;
  a2ui::AgentCallDone done() {
    return [this](Result<Json> r) {
      ++count;
      ok = r.ok();
      if (r.ok()) {
        value = std::move(r).value();
      } else {
        error = r.take_error();
      }
    };
  }
};

Json fetch_call() { return parse(R"({"@call":"fetch","catalogId":"c","args":{}})"); }

/** A processor with one v1.0 catalog "c" (no functions) and a recording listener. */
struct Fixture {
  std::vector<Json> outbound;
  std::vector<Json> errors;
  std::unique_ptr<MessageProcessor> processor;
  a2ui::SurfaceModel* surface = nullptr;
  a2ui::Subscription error_sub;

  explicit Fixture(a2ui::RpcOptions rpc = {}) {
    auto cat = std::make_shared<a2ui::Catalog>();
    cat->set_id("c");
    cat->set_protocol_version("1.0");
    a2ui::MessageProcessorOptions opts;
    opts.outbound_listener = [this](const Json& m) { outbound.push_back(m); };
    opts.rpc = std::move(rpc);
    processor = std::make_unique<MessageProcessor>(std::vector<a2ui::CatalogPtr>{cat}, std::move(opts));
    processor->process_messages(parse(R"({"version":"1.0","createSurface":{"surfaceId":"s","catalogId":"c"}})"));
    surface = processor->model().get_surface("s");
    error_sub = surface->on_error().subscribe([this](const Json& e) { errors.push_back(e); });
  }

  std::vector<std::string> call_ids() const {
    std::vector<std::string> ids;
    for (const auto& m : outbound) {
      if (m.contains("callAgentFunction")) ids.push_back(m["callAgentFunction"]["functionCallId"].as_string());
    }
    return ids;
  }

  void respond(const std::string& id, const char* value_json) {
    Json body = Json::object({{"functionCallId", Json(id)}, {"value", parse(value_json)}});
    processor->process_messages(Json::object({{"version", Json("1.0")}, {"agentFunctionResponse", body}}));
  }
};

// ---------------------------------------------------------------------------
// Timeouts

TEST(rpc_default_timeout_fires_at_deadline) {
  a2ui::RpcOptions o;
  o.default_timeout_ms = 30000;
  RpcHandler rpc({}, o);
  rpc.set_outbound_listener([](const Json&) {});
  rpc.tick(5000);
  Settled s;
  EXPECT_TRUE(rpc.call_agent_function("s", fetch_call(), s.done()).ok());
  rpc.tick(34999);
  EXPECT_EQ(s.count, 0);
  EXPECT_EQ(rpc.pending_call_count(), 1u);
  rpc.tick(35000);
  EXPECT_EQ(s.count, 1);
  EXPECT_TRUE(!s.ok);
  EXPECT_EQ(s.error.code, std::string("TIMEOUT"));
  EXPECT_EQ(s.error.kind, a2ui::ErrorKind::Rpc);
  EXPECT_TRUE(!s.error.function_call_id.empty());
  EXPECT_EQ(rpc.pending_call_count(), 0u);
  rpc.tick(99999);
  EXPECT_EQ(s.count, 1);  // settles once
}

TEST(rpc_per_call_timeout_and_disabled_timeout) {
  RpcHandler rpc;
  rpc.set_outbound_listener([](const Json&) {});
  rpc.tick(0);
  Settled short_call, no_timeout;
  AgentCallOptions a;
  a.timeout_ms = 10;
  AgentCallOptions b;
  b.timeout_ms = 0;
  EXPECT_TRUE(rpc.call_agent_function("s", fetch_call(), short_call.done(), a).ok());
  EXPECT_TRUE(rpc.call_agent_function("s", fetch_call(), no_timeout.done(), b).ok());
  rpc.tick(10);
  EXPECT_EQ(short_call.error.code, std::string("TIMEOUT"));
  rpc.tick(1000000000);
  EXPECT_EQ(no_timeout.count, 0);
  EXPECT_EQ(rpc.pending_call_count(), 1u);
}

TEST(rpc_timeout_starts_at_first_tick_without_clock) {
  // A host that starts ticking late (e.g. with an epoch-based time) must not
  // expire calls made before its first tick.
  RpcHandler rpc;
  rpc.set_outbound_listener([](const Json&) {});
  Settled s;
  AgentCallOptions o;
  o.timeout_ms = 100;
  EXPECT_TRUE(rpc.call_agent_function("s", fetch_call(), s.done(), o).ok());
  rpc.tick(1700000000000ull);
  EXPECT_EQ(s.count, 0);
  rpc.tick(1700000000100ull);
  EXPECT_EQ(s.error.code, std::string("TIMEOUT"));
}

TEST(rpc_clock_stamps_call_start) {
  uint64_t now = 500;
  a2ui::RpcOptions opts;
  opts.clock = [&now] { return now; };
  RpcHandler rpc({}, opts);
  rpc.set_outbound_listener([](const Json&) {});
  Settled s;
  AgentCallOptions o;
  o.timeout_ms = 100;
  EXPECT_TRUE(rpc.call_agent_function("s", fetch_call(), s.done(), o).ok());
  rpc.tick(599);
  EXPECT_EQ(s.count, 0);
  rpc.tick(600);
  EXPECT_EQ(s.error.code, std::string("TIMEOUT"));
}

TEST(rpc_late_response_after_timeout_is_ignored) {
  Fixture f;
  f.processor->tick(0);
  Settled s;
  AgentCallOptions o;
  o.function_call_id = "late-1";
  o.timeout_ms = 5;
  EXPECT_TRUE(f.processor->call_agent_function("s", fetch_call(), s.done(), o).ok());
  f.processor->tick(5);
  const size_t sent = f.outbound.size();
  f.respond("late-1", "42");
  EXPECT_EQ(s.count, 1);
  EXPECT_EQ(s.error.code, std::string("TIMEOUT"));
  EXPECT_EQ(f.outbound.size(), sent);
}

// ---------------------------------------------------------------------------
// Cancellation and disposal

TEST(rpc_cancel_surface_calls_is_surface_scoped) {
  RpcHandler rpc;
  rpc.set_outbound_listener([](const Json&) {});
  Settled a, b;
  EXPECT_TRUE(rpc.call_agent_function("s1", fetch_call(), a.done()).ok());
  EXPECT_TRUE(rpc.call_agent_function("s2", fetch_call(), b.done()).ok());
  rpc.cancel_surface_calls("s1");
  EXPECT_EQ(a.error.code, std::string("CANCELLED"));
  EXPECT_EQ(b.count, 0);
  EXPECT_EQ(rpc.pending_call_count(), 1u);
}

TEST(rpc_dispose_cancels_then_rejects) {
  RpcHandler rpc;
  rpc.set_outbound_listener([](const Json&) {});
  Settled a;
  EXPECT_TRUE(rpc.call_agent_function("s", fetch_call(), a.done()).ok());
  rpc.dispose();
  EXPECT_EQ(a.error.code, std::string("CANCELLED"));
  Settled b;
  auto r = rpc.call_agent_function("s", fetch_call(), b.done());
  EXPECT_TRUE(!r.ok());
  EXPECT_EQ(r.error().code, std::string("DISPOSED"));
  EXPECT_EQ(b.count, 0);
  EXPECT_TRUE(!rpc.can_call_agent());
}

TEST(rpc_callback_may_start_a_call_while_cancelling) {
  RpcHandler rpc;
  rpc.set_outbound_listener([](const Json&) {});
  Settled retry;
  bool retried = false;
  EXPECT_TRUE(rpc.call_agent_function("s", fetch_call(), [&](Result<Json>) {
                   retried = rpc.call_agent_function("s", fetch_call(), retry.done()).ok();
                 }).ok());
  rpc.cancel_surface_calls("s");
  EXPECT_TRUE(retried);
  EXPECT_EQ(rpc.pending_call_count(), 1u);
}

TEST(rpc_generated_ids_skip_explicit_ones) {
  RpcHandler rpc;
  rpc.set_outbound_listener([](const Json&) {});
  Settled a, b;
  AgentCallOptions o;
  o.function_call_id = "call-1";
  EXPECT_TRUE(rpc.call_agent_function("s", fetch_call(), a.done(), o).ok());
  auto r = rpc.call_agent_function("s", fetch_call(), b.done());
  EXPECT_TRUE(r.ok() && r.value() != "call-1");
}

// ---------------------------------------------------------------------------
// Reactive routing on a surface

/** Evaluates `{"@call":"fetch"}` as a reactive consumer would. */
Json evaluate(a2ui::AgentCallTracker& tracker, a2ui::SurfaceModel& s, bool* deferred = nullptr,
              bool* failed = nullptr) {
  tracker.begin_pass();
  a2ui::DataContext ctx(s.data_model(), &tracker, "/");
  Json v = ctx.resolve_value(parse(R"({"@call":"fetch","args":{"n":1}})"));
  if (deferred) *deferred = ctx.had_deferred();
  if (failed) *failed = ctx.had_failure();
  return v;
}

TEST(rpc_reactive_timeout_reports_and_is_remembered_per_consumer) {
  Fixture f;
  f.processor->tick(0);
  int settled = 0;
  bool last_answered = true;
  auto sub = f.surface->on_agent_call_settled().subscribe([&](const a2ui::AgentCallSettlement& s) {
    ++settled;
    last_answered = s.answered;
  });
  a2ui::AgentCallTracker a(*f.surface);
  bool deferred = false, failed = false;
  evaluate(a, *f.surface, &deferred, &failed);
  EXPECT_TRUE(deferred && !failed);
  EXPECT_EQ(f.call_ids().size(), 1u);

  f.processor->tick(30000);  // default timeout
  EXPECT_EQ(settled, 1);
  EXPECT_TRUE(!last_answered);
  EXPECT_EQ(f.errors.size(), 1u);
  EXPECT_EQ(f.errors[0]["code"].as_string(), std::string("EXPRESSION_ERROR"));
  EXPECT_EQ(f.errors[0]["functionCallId"].as_string(), f.call_ids()[0]);

  // The consumer that saw it fail reads it as failed, silently.
  evaluate(a, *f.surface, &deferred, &failed);
  EXPECT_TRUE(failed);
  EXPECT_EQ(f.call_ids().size(), 1u);
  EXPECT_EQ(f.errors.size(), 1u);
  // A consumer making the call anew sends it again.
  a2ui::AgentCallTracker b(*f.surface);
  evaluate(b, *f.surface, &deferred, &failed);
  EXPECT_TRUE(deferred && !failed);
  EXPECT_EQ(f.call_ids().size(), 2u);
}

TEST(rpc_reactive_cancel_on_live_surface_is_not_a_failure) {
  Fixture f;
  int settled = 0;
  auto sub = f.surface->on_agent_call_settled().subscribe([&](const a2ui::AgentCallSettlement&) { ++settled; });
  a2ui::AgentCallTracker a(*f.surface);
  evaluate(a, *f.surface);
  f.processor->rpc().cancel_surface_calls("s");
  EXPECT_EQ(settled, 0);
  EXPECT_TRUE(f.errors.empty());
  bool deferred = false;
  evaluate(a, *f.surface, &deferred);  // the same consumer sends it again
  EXPECT_TRUE(deferred);
  EXPECT_EQ(f.call_ids().size(), 2u);
  f.respond(f.call_ids()[1], "7");
  EXPECT_EQ(settled, 1);
  EXPECT_EQ(evaluate(a, *f.surface), Json(7));
}

TEST(rpc_delete_surface_cancels_silently) {
  Fixture f;
  a2ui::AgentCallTracker a(*f.surface);
  evaluate(a, *f.surface);
  int done = 0;
  f.surface->run_action(parse(R"({"functionCall":{"@call":"submit","args":{}}})"), "", "/", true,
                        [&](const Json&) { ++done; });
  EXPECT_EQ(f.processor->rpc().pending_call_count(), 2u);
  const auto ids = f.call_ids();
  f.processor->process_messages(parse(R"({"version":"1.0","deleteSurface":{"surfaceId":"s"}})"));
  EXPECT_EQ(f.processor->rpc().pending_call_count(), 0u);
  EXPECT_TRUE(f.errors.empty());
  EXPECT_EQ(done, 0);
  const size_t sent = f.outbound.size();
  f.respond(ids[1], "true");  // late: ignored
  EXPECT_EQ(f.outbound.size(), sent);
}

TEST(rpc_listener_may_answer_synchronously) {
  Fixture f;
  MessageProcessor* p = f.processor.get();
  p->set_outbound_listener([p](const Json& m) {
    if (!m.contains("callAgentFunction")) return;
    Json body = Json::object({{"functionCallId", m["callAgentFunction"]["functionCallId"]}, {"value", Json("hi")}});
    p->process_messages(Json::object({{"version", Json("1.0")}, {"agentFunctionResponse", body}}));
  });
  a2ui::AgentCallTracker a(*f.surface);
  bool deferred = true;
  EXPECT_EQ(evaluate(a, *f.surface, &deferred), Json("hi"));
  EXPECT_TRUE(!deferred);
  EXPECT_EQ(p->rpc().pending_call_count(), 0u);
}

TEST(rpc_no_listener_fails_locally) {
  Fixture f;
  f.processor->set_outbound_listener(nullptr);
  EXPECT_TRUE(!f.surface->routes_to_agent());
  a2ui::AgentCallTracker a(*f.surface);
  bool failed = false;
  evaluate(a, *f.surface, nullptr, &failed);
  EXPECT_TRUE(failed);
  EXPECT_EQ(f.errors.size(), 1u);
  EXPECT_EQ(f.errors[0]["code"].as_string(), std::string("EXPRESSION_ERROR"));
}

// ---------------------------------------------------------------------------
// Actions

TEST(rpc_action_timeout_reports_once_and_never_completes) {
  Fixture f;
  f.processor->tick(0);
  int done = 0;
  // Two sibling agent calls in the arguments of an outer agent call.
  f.surface->run_action(parse(R"({"functionCall":{"@call":"outer","args":{
      "a":{"@call":"x","args":{}},"b":{"@call":"y","args":{}}}}})"),
                        "", "/", true, [&](const Json&) { ++done; });
  EXPECT_EQ(f.call_ids().size(), 2u);
  f.processor->tick(30000);
  EXPECT_EQ(f.errors.size(), 1u);  // the first failure only
  EXPECT_EQ(f.errors[0]["code"].as_string(), std::string("EXPRESSION_ERROR"));
  EXPECT_EQ(f.call_ids().size(), 2u);  // `outer` never sent
  EXPECT_EQ(done, 0);
}

TEST(rpc_action_resumes_after_arguments_settle) {
  Fixture f;
  Json result;
  int done = 0;
  f.surface->run_action(parse(R"({"functionCall":{"@call":"outer","args":{"a":{"@call":"x","args":{}}}}})"), "",
                        "/", true, [&](const Json& r) {
                          ++done;
                          result = r;
                        });
  f.respond(f.call_ids()[0], "1");
  EXPECT_EQ(f.call_ids().size(), 2u);
  EXPECT_EQ(f.outbound.back()["callAgentFunction"]["callFunction"]["args"], parse(R"({"a":1})"));
  EXPECT_EQ(done, 0);
  f.respond(f.call_ids()[1], R"({"ok":true})");
  EXPECT_EQ(done, 1);
  EXPECT_EQ(result, parse(R"({"ok":true})"));
}

// ---------------------------------------------------------------------------
// AgentCallCache

TEST(agent_call_cache_evicts_oldest_settled_not_pending_or_in_use) {
  a2ui::AgentCallCache c;
  c.set_max_settled(2);
  c.mark_pending("p");
  c.answer("a", Json(1));
  c.answer("b", Json(2));
  c.set_in_use([](const std::string& k) { return k == "a"; });
  c.answer("c", Json(3));  // full: evicts "b" (oldest settled not in use)
  EXPECT_TRUE(c.find("p") != nullptr);
  EXPECT_TRUE(c.find("a") != nullptr);
  EXPECT_TRUE(c.find("b") == nullptr);
  EXPECT_TRUE(c.find("c") != nullptr);
  c.fail("a");
  c.forget_failure("a");
  EXPECT_TRUE(c.find("a") == nullptr);
  EXPECT_TRUE(c.has_pending());
}

TEST(agent_call_key_is_canonical) {
  a2ui::AgentFunctionCall a{"c", "f", parse(R"({"x":1,"y":[1,{"b":2,"a":1}]})")};
  a2ui::AgentFunctionCall b{"c", "f", parse(R"({"y":[1,{"a":1,"b":2}],"x":1})")};
  a2ui::AgentFunctionCall c{"c", "f", parse(R"({"y":[{"a":1,"b":2},1],"x":1})")};
  EXPECT_EQ(a.key(), b.key());
  EXPECT_TRUE(a.key() != c.key());  // array order kept
}

}  // namespace

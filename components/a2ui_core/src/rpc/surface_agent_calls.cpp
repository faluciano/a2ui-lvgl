// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Agent function routing on a surface (blueprint section D, "Renderer
// Fallback Routing"): the reactive call cache of SurfaceModel,
// AgentCallTracker, and SurfaceModel::run_action. Port of the agent-call
// parts of swift/core/Sources/A2UICore/Models/SurfaceViewModel.swift and
// Resolution/NodeResolver.swift (runActionFunctionCall, runEventAction).

#include <memory>
#include <utility>

#include "a2ui/agent_call.h"
#include "a2ui/data_context.h"
#include "a2ui/surface_model.h"

namespace a2ui {
namespace {

bool is_cancelled(const Error& e) { return e.code == rpc_code::kCancelled; }

/** Reports an agent error or timeout: always EXPRESSION_ERROR, with the agent's message and the call id. */
void report_agent_failure(SurfaceModel& surface, const AgentFunctionCall& call, const Error& rpc) {
  Error e = make_error(ErrorKind::Expression, rpc.message, "EXPRESSION_ERROR");
  e.function_call_id = rpc.function_call_id;
  surface.report_function_error(e, call.name);
}

/** Reports a call that could not be sent at all. */
void report_send_failure(SurfaceModel& surface, const AgentFunctionCall& call, const Error& rpc) {
  Error e = make_error(ErrorKind::Expression,
                       "Function '" + call.name + "' could not be sent to the agent: " + rpc.message,
                       "EXPRESSION_ERROR");
  surface.report_function_error(e, call.name);
}

std::optional<long> index_of_scope(const std::string& scope_path) {
  const auto segments = path::split(scope_path);
  if (!segments.empty()) {
    const long i = path::parse_index(segments.back());
    if (i >= 0) return i;
  }
  return std::nullopt;
}

/**
 * Why `value` is not a well-formed v1.0 dynamic value, or empty. An object
 * key starting with '@' must be @path or @call ('@@' escapes); a binding's
 * path and a call's name must be strings, and a call's args an object.
 */
std::string invalid_dynamic_value_reason(const Json& value) {
  if (value.is_array()) {
    for (const auto& e : value.as_array()) {
      std::string r = invalid_dynamic_value_reason(e);
      if (!r.empty()) return r;
    }
    return {};
  }
  if (!value.is_object()) return {};
  for (const auto& m : value.as_object()) {
    if (DataContext::is_invalid_reserved_key(m.first)) return "'" + m.first + "' is not a known expression key.";
  }
  if (const Json* p = value.find("@path"); p && !p->is_string()) return "'@path' must be a string.";
  if (const Json* c = value.find("@call")) {
    if (!c->is_string()) return "'@call' must be a string.";
    if (const Json* a = value.find("args"); a && !a->is_object()) {
      return "the 'args' of '" + c->as_string() + "' must be an object.";
    }
  }
  for (const auto& m : value.as_object()) {
    std::string r = invalid_dynamic_value_reason(m.second);
    if (!r.empty()) return r;
  }
  return {};
}

// ---------------------------------------------------------------------------
// Actions

/** One run of an action: its own call cache, never shared with the surface's. */
struct ActionRun {
  AgentCallCache cache;
  bool stopped = false;
  /** The latest call requested during a pass (the evaluated call itself when no argument deferred). */
  std::string last_key;
  /** Continues the action once every pending call is answered. */
  std::function<void()> resume;
  /** Optional action.metadata envelope attached when the event is dispatched. */
  Json metadata;

  void stop() {
    stopped = true;
    resume = nullptr;
  }
};

void settle_action_call(SurfaceModel& surface, ActionRun& run, const AgentFunctionCall& call,
                        const std::string& key, Result<Json> outcome) {
  if (surface.disposed()) return;
  if (outcome.ok()) {
    run.cache.answer(key, std::move(outcome).value());
    if (!run.stopped && !run.cache.has_pending() && run.resume) {
      auto resume = run.resume;
      resume();
    }
    return;
  }
  const bool was_stopped = run.stopped;
  run.cache.fail(key);
  run.stop();
  // The action reports its first failure only; a cancellation reports nothing.
  if (was_stopped || is_cancelled(outcome.error())) return;
  report_agent_failure(surface, call, outcome.error());
}

/** The FunctionHost of an action run: the surface, except that agent calls go through the run's cache. */
class ActionHost : public FunctionHost {
 public:
  ActionHost(SurfaceModel& surface, Ref<ActionRun> run) : surface_(surface), run_(std::move(run)) {}

  Result<const Catalog*> resolve_function_catalog(const std::string* catalog_id,
                                                  const std::string& function_name) override {
    return surface_.resolve_function_catalog(catalog_id, function_name);
  }
  void report_function_error(const Error& error, const std::string& function_name) override {
    surface_.report_function_error(error, function_name);
  }
  Status validate_function_args(const FunctionApi& api, const Json& args, ArgsCheck check) override {
    return surface_.validate_function_args(api, args, check);
  }
  bool routes_to_agent() const override { return surface_.routes_to_agent(); }
  Status open_url(const std::string& url) override { return surface_.open_url(url); }

  AgentCallStatus agent_function_value(const std::string& name, const std::string& catalog_id, const Json& args,
                                       Json* value) override {
    AgentFunctionCall call{catalog_id, name, args};
    const std::string key = call.key();
    run_->last_key = key;
    if (const auto* e = run_->cache.find(key)) {
      switch (e->state) {
        case AgentCallCache::State::Answered: *value = e->value; return AgentCallStatus::Answered;
        case AgentCallCache::State::Pending: return AgentCallStatus::Pending;
        case AgentCallCache::State::Failed: return AgentCallStatus::Failed;
      }
    }
    AgentFunctionCaller* caller = surface_.agent_function_caller();
    if (!caller) {
      report_send_failure(surface_, call, make_error(ErrorKind::Rpc, "no agent function caller", rpc_code::kNoListener));
      return AgentCallStatus::Failed;
    }
    run_->cache.mark_pending(key);
    WeakRef<const void> alive = surface_.liveness();
    SurfaceModel* surface = &surface_;
    Ref<ActionRun> run = run_;
    auto sent = caller->call_agent(surface_.id(), call, [alive, surface, run, call, key](Result<Json> outcome) {
      if (alive.expired()) return;
      settle_action_call(*surface, *run, call, key, std::move(outcome));
    });
    if (!sent.ok()) {
      run_->cache.forget(key);
      report_send_failure(surface_, call, sent.error());
      return AgentCallStatus::Failed;
    }
    if (const auto* e = run_->cache.find(key)) {  // answered synchronously?
      if (e->state == AgentCallCache::State::Answered) {
        *value = e->value;
        return AgentCallStatus::Answered;
      }
      if (e->state == AgentCallCache::State::Failed) return AgentCallStatus::Failed;
    }
    return AgentCallStatus::Pending;
  }

 private:
  SurfaceModel& surface_;
  Ref<ActionRun> run_;
};

using DoneFn = std::function<void(const Json&)>;

void run_function_call(SurfaceModel& surface, const Ref<ActionRun>& run, const Json& call,
                       const std::string& scope, bool user_activated, const DoneFn& on_done) {
  if (run->stopped || surface.disposed()) return;
  ActionHost host(surface, run);
  DataContext ctx(surface.data_model(), &host, scope, index_of_scope(scope), user_activated);
  Json result = ctx.resolve_value(call);
  if (ctx.had_failure()) {
    run->stop();
    return;
  }
  if (!run->cache.has_pending()) {
    run->resume = nullptr;
    if (on_done) on_done(result);
    return;
  }
  WeakRef<ActionRun> weak = run;
  if (ctx.had_deferred_arguments()) {
    // The call runs once the agent calls in its arguments are answered.
    SurfaceModel* s = &surface;
    run->resume = [s, weak, call, scope, user_activated, on_done] {
      if (auto r = weak.lock()) run_function_call(*s, r, call, scope, user_activated, on_done);
    };
  } else {
    // The evaluated call itself was sent: its answer is the result.
    const std::string key = run->last_key;
    run->resume = [weak, key, on_done] {
      auto r = weak.lock();
      if (!r) return;
      r->resume = nullptr;
      const auto* e = r->cache.find(key);
      if (on_done && e && e->state == AgentCallCache::State::Answered) on_done(e->value);
    };
  }
}

Error invalid_action(const std::string& message) {
  return make_error(ErrorKind::Validation, message, "INVALID_ACTION");
}

void report_invalid_event_value(SurfaceModel& surface, const std::string& message) {
  surface.report_function_error(make_error(ErrorKind::Expression, message, "EXPRESSION_ERROR"), "");
}

/**
 * Resolves an event's context entries in order, then its userMessage; an
 * entry that waits on the agent is awaited before the next one is resolved.
 * Each run resolves the entries again, reading answers from the run cache.
 */
void run_event(SurfaceModel& surface, const Ref<ActionRun>& run, const Json& payload, bool shorthand,
               const std::string& source_id, const std::string& scope, bool user_activated, const DoneFn& on_done) {
  if (run->stopped || surface.disposed()) return;
  const Json& evt = shorthand ? payload : payload["event"];
  ActionHost host(surface, run);
  DataContext ctx(surface.data_model(), &host, scope, index_of_scope(scope), user_activated);

  SurfaceModel* s = &surface;
  WeakRef<ActionRun> weak = run;
  auto wait = [&] {
    run->resume = [s, weak, payload, shorthand, source_id, scope, user_activated, on_done] {
      if (auto r = weak.lock()) run_event(*s, r, payload, shorthand, source_id, scope, user_activated, on_done);
    };
  };

  Json resolved_context = Json::object();
  for (const auto& m : evt["context"].as_object()) {
    const std::string reason = invalid_dynamic_value_reason(m.second);
    if (!reason.empty()) {
      run->stop();
      report_invalid_event_value(surface, "Action event context entry '" + m.first + "' is invalid: " + reason);
      return;
    }
    ctx.reset_flags();
    Json v = ctx.resolve_value(m.second);
    if (ctx.had_failure()) {
      run->stop();
      return;
    }
    if (run->cache.has_pending()) {
      wait();
      return;
    }
    resolved_context.set(m.first, std::move(v));
  }

  Json user_message;
  if (const Json* um = evt.find("userMessage"); um && !um->is_undefined()) {
    const std::string reason = invalid_dynamic_value_reason(*um);
    if (!reason.empty()) {
      run->stop();
      report_invalid_event_value(surface, "Action event userMessage is invalid: " + reason);
      return;
    }
    ctx.reset_flags();
    Json v = ctx.resolve_value(*um);
    if (ctx.had_failure()) {
      run->stop();
      return;
    }
    if (run->cache.has_pending()) {
      wait();
      return;
    }
    if (!v.is_nullish() && !v.is_string()) {
      run->stop();
      surface.report_function_error(invalid_action("Action event 'userMessage' must resolve to a string."), "");
      return;
    }
    user_message = std::move(v);  // null: absent
  }

  run->resume = nullptr;
  Json out_evt = evt;
  out_evt.set("context", std::move(resolved_context));
  if (user_message.is_string()) {
    out_evt.set("userMessage", std::move(user_message));
  } else {
    out_evt.erase("userMessage");
  }
  Json out = payload;
  if (shorthand) {
    out = std::move(out_evt);
  } else {
    out.set("event", std::move(out_evt));
  }
  if (!surface.dispatch_action(out, source_id, run->metadata).ok()) return;
  if (on_done) on_done(out);
}

}  // namespace

// ---------------------------------------------------------------------------
// SurfaceModel: reactive agent calls

void SurfaceModel::set_agent_function_caller(AgentFunctionCaller* caller) {
  if (disposed_ || caller == agent_caller_) return;
  agent_caller_ = caller;
  ++agent_generation_;
  agent_cache_.clear();
}

bool SurfaceModel::routes_to_agent() const {
  return !disposed_ && is_v1() && agent_caller_ && agent_caller_->can_call_agent();
}

AgentCallStatus SurfaceModel::agent_function_value(const std::string& name, const std::string& catalog_id,
                                                   const Json& args, Json* value) {
  AgentFunctionCall call{catalog_id, name, args};
  if (disposed_ || !agent_caller_) {
    report_function_error(function_errors::not_found(name), name);
    return AgentCallStatus::Failed;
  }
  const std::string key = call.key();
  if (const auto* e = agent_cache_.find(key)) {
    switch (e->state) {
      case AgentCallCache::State::Answered: *value = e->value; return AgentCallStatus::Answered;
      case AgentCallCache::State::Pending: return AgentCallStatus::Pending;
      case AgentCallCache::State::Failed: return AgentCallStatus::Failed;
    }
  }

  agent_cache_.mark_pending(key);
  WeakRef<int> alive = alive_;
  const uint32_t generation = agent_generation_;
  auto sent = agent_caller_->call_agent(id_, call, [this, alive, generation, call, key](Result<Json> outcome) {
    // Ignore calls that settle after the surface is gone or its caller changed.
    if (alive.expired() || generation != agent_generation_) return;
    settle_agent_call(call, key, std::move(outcome));
  });
  if (!sent.ok()) {
    const auto* e = agent_cache_.find(key);
    if (e && e->state == AgentCallCache::State::Pending) agent_cache_.forget(key);
    report_send_failure(*this, call, sent.error());
    return AgentCallStatus::Failed;
  }
  if (const auto* e = agent_cache_.find(key)) {  // answered synchronously?
    if (e->state == AgentCallCache::State::Answered) {
      *value = e->value;
      return AgentCallStatus::Answered;
    }
    if (e->state == AgentCallCache::State::Failed) return AgentCallStatus::Failed;
  }
  return AgentCallStatus::Pending;
}

void SurfaceModel::settle_agent_call(const AgentFunctionCall& call, const std::string& key, Result<Json> outcome) {
  if (disposed_) return;
  AgentCallSettlement settlement{call, key, outcome.ok()};
  if (outcome.ok()) {
    agent_cache_.answer(key, std::move(outcome).value());
  } else if (is_cancelled(outcome.error())) {
    // A cancellation is not a failure: the next evaluation sends the call again.
    agent_cache_.forget(key);
    return;
  } else {
    agent_cache_.fail(key);
    report_agent_failure(*this, call, outcome.error());
    if (disposed_) return;
  }
  on_agent_call_settled_.emit(settlement);
}

void SurfaceModel::run_action(const Json& action, const std::string& source_component_id,
                              const std::string& scope_path, bool user_activated,
                              std::function<void(const Json& result)> on_done, const Json& metadata) {
  if (disposed_ || !action.is_object()) return;
  auto run = make_ref<ActionRun>();
  run->metadata = metadata;
  auto check_name = [&](const Json& evt) {
    const Json& name = evt["name"];
    if (name.is_string() && !name.as_string().empty()) return true;
    report_function_error(invalid_action("Action event is missing required string 'name'."), "");
    return false;
  };
  if (const Json* evt = action.find("event"); evt && evt->is_object()) {
    if (check_name(*evt)) run_event(*this, run, action, false, source_component_id, scope_path, user_activated, on_done);
    return;
  }
  if (action.contains("name")) {
    if (check_name(action)) run_event(*this, run, action, true, source_component_id, scope_path, user_activated, on_done);
    return;
  }
  const Json* call = nullptr;
  if (const Json* fc = action.find("functionCall"); fc && fc->is_object()) {
    call = fc;
  } else if (action["@call"].is_string()) {
    call = &action;
  }
  if (call) run_function_call(*this, run, *call, scope_path, user_activated, on_done);
}

// ---------------------------------------------------------------------------
// AgentCallTracker

void AgentCallTracker::begin_pass() {
  previous_ = std::move(current_);
  current_.clear();
}

Result<const Catalog*> AgentCallTracker::resolve_function_catalog(const std::string* catalog_id,
                                                                  const std::string& function_name) {
  return surface_->resolve_function_catalog(catalog_id, function_name);
}

void AgentCallTracker::report_function_error(const Error& error, const std::string& function_name) {
  surface_->report_function_error(error, function_name);
}

Status AgentCallTracker::validate_function_args(const FunctionApi& api, const Json& args, ArgsCheck check) {
  return surface_->validate_function_args(api, args, check);
}

bool AgentCallTracker::routes_to_agent() const { return surface_->routes_to_agent(); }

AgentCallStatus AgentCallTracker::agent_function_value(const std::string& name, const std::string& catalog_id,
                                                       const Json& args, Json* value) {
  const std::string key = AgentFunctionCall{catalog_id, name, args}.key();
  // A call this consumer makes anew is sent again even if it failed for another one.
  if (!previous_.count(key) && !current_.count(key)) surface_->agent_call_cache().forget_failure(key);
  current_.insert(key);
  return surface_->agent_function_value(name, catalog_id, args, value);
}

Status AgentCallTracker::open_url(const std::string& url) { return surface_->open_url(url); }

}  // namespace a2ui

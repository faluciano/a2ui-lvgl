// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Agent function calls routed from a surface (v1.0 "Function Location
// Resolution & Fallback Routing"; blueprint section D, "Renderer Fallback
// Routing"). Ported from the Swift core's Models/AgentCallCache.swift and
// the agent-call half of Models/SurfaceViewModel.swift.
//
// - AgentFunctionCall identifies a call by its resolved catalogId, name and
//   arguments (object keys in canonical order, array order kept).
// - AgentFunctionCaller is how a surface reaches the agent. RpcHandler
//   implements it; MessageProcessor installs it on every v1.0 surface.
// - AgentCallCache holds what a surface knows about the calls its reactive
//   evaluations (bindings, check rules) made: pending, answered or failed.
//
// No threads or timers: the caller reports each outcome through a callback
// on the thread that drives the processor (the LVGL task on device).

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <set>
#include <string>

#include "a2ui/data_context.h"
#include "a2ui/error.h"
#include "a2ui/json.h"

namespace a2ui {

class SurfaceModel;

/** RPC error codes (blueprint `RpcErrorCode`). */
namespace rpc_code {
constexpr const char* kInvalidFunctionCall = "INVALID_FUNCTION_CALL";
constexpr const char* kExecutionError = "EXECUTION_ERROR";
constexpr const char* kTimeout = "TIMEOUT";
constexpr const char* kCancelled = "CANCELLED";
constexpr const char* kDisposed = "DISPOSED";
constexpr const char* kDuplicate = "DUPLICATE";
constexpr const char* kNoListener = "NO_LISTENER";
}  // namespace rpc_code

/** JSON text with object keys sorted (array order kept): the identity of a value. */
std::string canonical_json(const Json& value);

/** A function call a surface sends to the agent. */
struct AgentFunctionCall {
  /** The catalog the call resolved to (always named on the wire). */
  std::string catalog_id;
  std::string name;
  /** Resolved arguments (an object). */
  Json args = Json::object();

  /** Identity of the call: equal for calls with equal catalog, name and arguments. */
  std::string key() const;
  /** The wire `callFunction` payload: {"@call", "catalogId", "args"}. */
  Json to_call_function() const;
};

/**
 * Receives the outcome of an agent call exactly once: the agent's value
 * (null when the response carried none), or an ErrorKind::Rpc error whose
 * `code` is the RPC code (the agent's own code for an agent error, TIMEOUT,
 * CANCELLED) and whose `function_call_id` names the call.
 */
using AgentCallDone = std::function<void(Result<Json> outcome)>;

/** How a surface reaches the agent. */
class AgentFunctionCaller {
 public:
  virtual ~AgentFunctionCaller() = default;
  /** Whether calls can reach the agent now (an outbound listener is set and the caller is live). */
  virtual bool can_call_agent() const = 0;
  /**
   * Sends `call` for `surface_id` and returns its functionCallId. `done`
   * runs once the call settles, possibly before this returns. On an error
   * (NO_LISTENER, DISPOSED, ...) nothing was sent and `done` never runs.
   */
  virtual Result<std::string> call_agent(const std::string& surface_id, const AgentFunctionCall& call,
                                         AgentCallDone done) = 0;
};

/** Published by a surface when a reactive agent call settles into its cache. */
struct AgentCallSettlement {
  AgentFunctionCall call;
  std::string key;
  /** True when the agent answered with a value; false after an agent error or a timeout. */
  bool answered = false;
};

/**
 * The state of the agent calls made by one owner: a surface's reactive
 * evaluations, or one run of an action.
 *
 * At most `max_settled()` settled entries are kept; the oldest settled entry
 * that is not in use is evicted first. A pending entry, or one `in_use`
 * reports, is never evicted.
 */
class AgentCallCache {
 public:
  enum class State : uint8_t { Pending, Answered, Failed };
  struct Entry {
    State state = State::Pending;
    Json value;  // Answered only
  };

  static constexpr size_t kDefaultMaxSettled = 256;

  /** The entry for a call key, or null. */
  const Entry* find(const std::string& key) const;
  void mark_pending(const std::string& key);
  void answer(const std::string& key, Json value);
  void fail(const std::string& key);
  /** Drops a Failed entry so that the next evaluation sends the call again. */
  void forget_failure(const std::string& key);
  /** Drops the entry whatever its state. */
  void forget(const std::string& key);
  void clear();

  bool has_pending() const;
  size_t size() const { return index_.size(); }

  size_t max_settled() const { return max_settled_; }
  void set_max_settled(size_t n) { max_settled_ = n == 0 ? 1 : n; }
  /** Tells the cache which settled entries a live consumer still reads. */
  void set_in_use(std::function<bool(const std::string& key)> in_use) { in_use_ = std::move(in_use); }

 private:
  using Order = std::list<std::pair<std::string, Entry>>;
  void put(const std::string& key, Entry entry);
  void evict_if_full();

  Order order_;  // insertion order, oldest first
  std::map<std::string, Order::iterator> index_;
  size_t max_settled_ = kDefaultMaxSettled;
  std::function<bool(const std::string&)> in_use_;
};

/**
 * The FunctionHost of one reactive consumer (a component property, a
 * binding, a check rule) on a surface: everything goes to the surface,
 * which shares pending calls and caches answers, and the tracker records
 * the agent calls each evaluation pass makes.
 *
 * Failure memory follows the blueprint: a consumer that saw a call fail
 * reads it as failed on re-evaluation (it is not sent again when an
 * unrelated path changes), while a consumer that makes the call for the
 * first time sends it again. Call begin_pass() before each evaluation, and
 * re-evaluate when on_agent_call_settled() names a key makes_call() reports.
 * The surface must outlive the tracker.
 */
class AgentCallTracker : public FunctionHost {
 public:
  explicit AgentCallTracker(SurfaceModel& surface) : surface_(&surface) {}

  /** Starts an evaluation pass. */
  void begin_pass();
  /** Whether the latest pass made the call (a settlement of it should re-evaluate). */
  bool makes_call(const std::string& key) const { return current_.count(key) > 0; }
  const std::set<std::string>& calls() const { return current_; }
  SurfaceModel& surface() const { return *surface_; }

  // FunctionHost (delegates to the surface)
  Result<const Catalog*> resolve_function_catalog(const std::string* catalog_id,
                                                  const std::string& function_name) override;
  void report_function_error(const Error& error, const std::string& function_name) override;
  Status validate_function_args(const FunctionApi& api, const Json& args, ArgsCheck check) override;
  bool routes_to_agent() const override;
  AgentCallStatus agent_function_value(const std::string& name, const std::string& catalog_id, const Json& args,
                                       Json* value) override;
  Status open_url(const std::string& url) override;

 private:
  SurfaceModel* surface_;
  std::set<std::string> previous_;
  std::set<std::string> current_;
};

}  // namespace a2ui

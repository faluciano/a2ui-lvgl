// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// The bidirectional RPC layer (blueprint section D, "RPC Layer" and "RPC
// Execution Lifecycle & Safeguards"). Ported from the Swift core's
// RPC/RPCHandler.swift, with the lifecycle semantics of the Python SDK's
// rpc_handler.py (timeouts, cancellation, disposal, DUPLICATE/NO_LISTENER).
//
// MessageProcessor owns one RpcHandler (processor.rpc()). Inbound
// callRendererFunction and agentFunctionResponse envelopes reach it through
// process_messages; outbound callAgentFunction calls come from surfaces
// (fallback routing) or from the host (processor.call_agent_function).
//
// Time: the handler starts no timers. The host calls tick(now_ms) (usually
// processor.tick) from its loop, on device from the LVGL task, and calls
// whose deadline has passed fail with TIMEOUT then. A call made before the
// first tick starts its timeout at that tick, unless RpcOptions::clock is set.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "a2ui/agent_call.h"
#include "a2ui/catalog.h"
#include "a2ui/error.h"
#include "a2ui/json.h"

namespace a2ui {

struct InternalOperation;
class SurfaceModel;

/** Receives renderer-to-agent messages ({version, error} envelopes, RPC calls and responses). */
using OutboundListener = std::function<void(const Json& message)>;

struct RpcOptions {
  /** Timeout of an outbound call that names none; 0 disables it. */
  uint32_t default_timeout_ms = 30000;
  /**
   * Monotonic milliseconds, read when a call starts. Optional: without it a
   * call's timeout starts at the latest tick(now_ms).
   */
  std::function<uint64_t()> clock;
};

/** Options of one outbound call (blueprint `CallOptions`). */
struct AgentCallOptions {
  /** Empty to generate one; a functionCallId still pending is a DUPLICATE. */
  std::string function_call_id;
  /** Unset for RpcOptions::default_timeout_ms; 0 disables the timeout. */
  std::optional<uint32_t> timeout_ms;
  std::string version = "1.0";
};

class RpcHandler : public AgentFunctionCaller {
 public:
  explicit RpcHandler(std::vector<CatalogPtr> catalogs = {}, RpcOptions options = {});
  /** Drops pending calls without running their callbacks (dispose() first to fail them). */
  ~RpcHandler() override;
  RpcHandler(const RpcHandler&) = delete;
  RpcHandler& operator=(const RpcHandler&) = delete;

  /** The catalogs inbound calls resolve against after the target surface's own. */
  void set_catalogs(std::vector<CatalogPtr> catalogs) { catalogs_ = std::move(catalogs); }
  const std::vector<CatalogPtr>& catalogs() const { return catalogs_; }

  void set_outbound_listener(OutboundListener listener) { outbound_ = std::move(listener); }
  bool has_outbound_listener() const { return static_cast<bool>(outbound_); }

  // -- Outbound calls (renderer -> agent) ------------------------------------

  /**
   * Sends {version, callAgentFunction: {surfaceId, functionCallId,
   * callFunction}} to the outbound listener and returns the functionCallId.
   * `done` runs exactly once when the agent answers, the call times out
   * (TIMEOUT), or it is cancelled (CANCELLED). Fails synchronously, without
   * sending or calling `done`, with ErrorKind::Rpc and code DISPOSED,
   * NO_LISTENER, DUPLICATE or INVALID_FUNCTION_CALL (no `@call` name).
   */
  Result<std::string> call_agent_function(const std::string& surface_id, const Json& call_function,
                                          AgentCallDone done, const AgentCallOptions& options = {});

  /** Advances the handler's time and fails every call whose deadline has passed with TIMEOUT. */
  void tick(uint64_t now_ms);
  /** The time of the latest tick (or the clock's, when one is set). */
  uint64_t now_ms() const;

  size_t pending_call_count() const { return pending_.size(); }
  bool is_pending(const std::string& function_call_id) const { return pending_.count(function_call_id) > 0; }

  /** Fails every pending call made for `surface_id` with CANCELLED. */
  void cancel_surface_calls(const std::string& surface_id);
  /** Fails every pending call with CANCELLED; the handler stays usable. */
  void cancel_all_calls();
  /**
   * Fails every pending call with CANCELLED, drops the outbound listener and
   * makes later calls fail with DISPOSED (inbound calls are answered with
   * DISPOSED errors, which go nowhere without a listener).
   */
  void dispose(const std::string& reason = "The RPC handler was disposed.");
  bool disposed() const { return disposed_; }

  // -- Inbound messages (called by MessageProcessor) --------------------------

  /**
   * Runs an agent's callRendererFunction against `surface` (null when the
   * call names no surface, or one that does not exist) and sends the
   * {version, rendererFunctionResponse} reply to the outbound listener.
   * Returns the reply.
   */
  Json handle_call_renderer_function(const InternalOperation& op, SurfaceModel* surface, bool user_activated);
  /**
   * Settles the pending call an agentFunctionResponse answers. A response
   * whose functionCallId matches no pending call (unknown, late, duplicate)
   * is ignored; returns whether one was settled.
   */
  bool handle_agent_function_response(const InternalOperation& op);

  // -- AgentFunctionCaller (fallback routing from surfaces) -------------------
  bool can_call_agent() const override { return !disposed_ && has_outbound_listener(); }
  Result<std::string> call_agent(const std::string& surface_id, const AgentFunctionCall& call,
                                 AgentCallDone done) override;

 private:
  struct Pending {
    std::string surface_id;
    std::string name;
    AgentCallDone done;
    uint64_t seq = 0;
    uint32_t timeout_ms = 0;  // 0: none
    bool started = false;     // start_ms is known
    uint64_t start_ms = 0;
  };

  std::string next_call_id();
  void send(const Json& message) const;
  /** Removes the pending calls matching `pred` and fails them, oldest first. */
  void reject_where(const std::function<bool(const std::string& id, const Pending&)>& pred,
                    const std::function<Error(const std::string& id, const Pending&)>& make_error);

  std::vector<CatalogPtr> catalogs_;
  RpcOptions options_;
  OutboundListener outbound_;
  std::map<std::string, Pending> pending_;
  uint64_t seq_ = 0;
  uint64_t id_counter_ = 0;
  bool ticked_ = false;
  uint64_t last_tick_ms_ = 0;
  bool disposed_ = false;
};

/** An ErrorKind::Rpc error with a wire code. */
Error rpc_error(const char* code, std::string message, std::string function_call_id = {});

}  // namespace a2ui

// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// SurfaceModel and SurfaceGroupModel (blueprint section E, "State Models"),
// ported from the Swift core's Models/SurfaceGroupModel.swift and the state
// half of Models/SurfaceViewModel.swift.
//
// A surface holds a set of catalogs: `default_catalog` (createSurface's
// catalogId, possibly none from v1.0) decides what an unqualified reference
// means, and `available_catalogs` (the processor's catalogs compatible with
// the surface version) is what a catalogId override may name.
// resolve_catalog() is the one implementation of that rule; validation and
// DataContext dispatch (through FunctionHost) both use it.

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "a2ui/agent_call.h"
#include "a2ui/catalog.h"
#include "a2ui/component_model.h"
#include "a2ui/data_context.h"
#include "a2ui/data_model.h"
#include "a2ui/error.h"
#include "a2ui/events.h"
#include "a2ui/json.h"

namespace a2ui {

using CatalogMap = std::map<std::string, CatalogPtr, std::less<>>;

/** Handles openUrl. Return an error to refuse. */
using UrlHandler = std::function<Status(const std::string& url)>;

struct SurfaceOptions {
  /** Normalized protocol version ("1.0"); defaults to the default catalog's. */
  std::string protocol_version;
  bool send_data_model = false;
  std::string root_id = "root";
  /** createSurface.metadata; must already be valid (see validate_metadata). */
  Json metadata;
  /** Initial data model document. */
  Json data_model;
};

class SurfaceModel : public FunctionHost {
 public:
  SurfaceModel(std::string id, CatalogPtr default_catalog, CatalogMap available_catalogs,
               SurfaceOptions options = {});
  ~SurfaceModel() override;
  SurfaceModel(const SurfaceModel&) = delete;
  SurfaceModel& operator=(const SurfaceModel&) = delete;

  const std::string& id() const { return id_; }
  /** The catalog createSurface named; null on a v1.0 surface created without one. */
  const Catalog* default_catalog() const { return default_catalog_.get(); }
  const CatalogPtr& default_catalog_ptr() const { return default_catalog_; }
  const CatalogMap& available_catalogs() const { return available_catalogs_; }

  /**
   * The catalog an item runs in: the one `catalog_id` names (from
   * available_catalogs; an empty string names a catalog too), else the
   * default. ErrorKind::Catalog when the named catalog is unavailable or
   * nothing is named and there is no default. `subject` ("Function call
   * 'upper'") prefixes the no-default message.
   */
  Result<CatalogPtr> resolve_catalog(const std::string* catalog_id, std::string_view subject = "Item") const;

  DataModel& data_model() { return data_model_; }
  const DataModel& data_model() const { return data_model_; }
  SurfaceComponentsModel& components_model() { return components_; }
  const SurfaceComponentsModel& components_model() const { return components_; }

  const std::string& root_id() const { return root_id_; }
  const std::string& protocol_version() const { return protocol_version_; }
  bool is_v1() const { return protocol_version_ == "1.0"; }
  bool send_data_model() const { return send_data_model_; }

  // MetadataHolder (createSurface.metadata).
  const Json& metadata() const { return metadata_.value(); }
  const Json& extensions() const { return metadata_.value()["extensions"]; }
  Status set_metadata(const Json& metadata);
  Status set_extension(const std::string& key, Json value);
  void remove_extension(const std::string& key);
  const Signal<Json>& metadata_signal() const { return metadata_; }
  const EventSource<Json>& on_metadata_changed() const { return on_metadata_changed_; }

  /** Renderer actions ({name, surfaceId, sourceComponentId, timestamp, context, ...}). */
  const EventSource<Json>& on_action() const { return on_action_; }
  /** Error payloads ({code, message, surfaceId | functionCallId, path?}). */
  const EventSource<Json>& on_error() const { return on_error_; }
  /** Evaluation failures as typed errors, before they are reported on on_error(). */
  const EventSource<Error>& on_function_error() const { return on_function_error_; }

  /**
   * A DataContext over this surface's data model at `path`, with this
   * surface as its FunctionHost.
   */
  DataContext data_context(const std::string& path = "/", bool user_activated = false);

  /**
   * Resolves an Action (blueprint "Action"): for an event action
   * ({event: {name, context?, userMessage?}} or the {name, ...} shorthand)
   * returns the action with `context` and `userMessage` resolved; for a
   * function call action ({functionCall: {...}} or a bare call) runs the
   * call with `user_activated` and returns its result. Returns null when
   * the action failed; the failure was reported on on_error()
   * (INVALID_ACTION for a malformed event).
   *
   * Synchronous: it cannot wait for the agent. A call routed to the agent
   * reads as pending (null) through the reactive cache. Run actions that
   * may call the agent with run_action().
   */
  Json resolve_action(const Json& action, const std::string& scope_path = "/", bool user_activated = true);

  /**
   * Emits a renderer action from a resolved event action. `metadata` is an
   * optional action.metadata envelope (merged over one in the payload);
   * its extension keys must be UAX #31 identifiers (ErrorKind::Validation).
   * A payload that is not an event action emits nothing.
   */
  Status dispatch_action(const Json& payload, const std::string& source_component_id,
                         const Json& metadata = Json());

  /** Emits an error payload on on_error(), stamping surfaceId unless it carries functionCallId. */
  void dispatch_error(Json error);

  /** Clock used for action timestamps (seconds since the epoch); the default reads the system time. */
  void set_clock(std::function<double()> clock) { clock_ = std::move(clock); }

  // -- Agent function routing (v1.0; blueprint "Renderer Fallback Routing") --
  // Implemented in src/rpc/surface_agent_calls.cpp.

  /**
   * Sends calls to functions the renderer does not implement to the agent.
   * MessageProcessor installs its RpcHandler on every v1.0 surface. Not
   * owned. Setting, replacing or clearing it drops the shared calls and the
   * cache; answers to calls made through a previous caller are ignored.
   */
  void set_agent_function_caller(AgentFunctionCaller* caller);
  AgentFunctionCaller* agent_function_caller() const { return agent_caller_; }

  /**
   * The calls the surface's reactive evaluations (bindings, check rules)
   * routed to the agent: identical calls share one request while pending,
   * answers are kept (bounded, see AgentCallCache), and failures are kept
   * so that re-evaluating a binding does not send a failed call again. A
   * consumer that should send a failed call again when it first makes it
   * evaluates through an AgentCallTracker.
   */
  AgentCallCache& agent_call_cache() { return agent_cache_; }
  /**
   * Fires after a reactive agent call settles into agent_call_cache() with
   * the agent's value or a failure (already reported on on_error()).
   * Evaluations that made the call re-evaluate. A cancelled call settles
   * into nothing and fires nothing.
   */
  const EventSource<AgentCallSettlement>& on_agent_call_settled() const { return on_agent_call_settled_; }

  /**
   * Runs an action in response to a user gesture, routing calls to the
   * agent as an action does: every call is sent afresh (never shared with
   * or read from the reactive cache), and the action waits for the answers.
   *
   * - A function call action ({functionCall: {...}} or a bare call) runs the
   *   call; arguments that wait on the agent are settled first. `on_done`
   *   receives the call's result.
   * - An event action ({event: {name, context?, userMessage?}} or the
   *   {name, ...} shorthand) resolves its `context` entries in order, then
   *   `userMessage`, waiting for each agent call before the next entry, and
   *   is then dispatched (on_action()). `on_done` receives the dispatched
   *   action payload.
   *
   * The first failure stops the action, is reported once on on_error(), and
   * `on_done` never runs; nor does it once the surface is deleted.
   * `metadata` is an optional action.metadata envelope for the dispatched event.
   */
  void run_action(const Json& action, const std::string& source_component_id = {},
                  const std::string& scope_path = "/", bool user_activated = true,
                  std::function<void(const Json& result)> on_done = {}, const Json& metadata = Json());

  /** Expires when the surface is disposed (for callbacks that outlive it). */
  std::weak_ptr<const void> liveness() const { return alive_; }
  void set_url_handler(UrlHandler handler) { url_handler_ = std::move(handler); }

  bool disposed() const { return disposed_; }
  /**
   * Fires once, at the start of dispose(), while the components and data
   * model are still intact (a NodeResolver tears its tree down here).
   */
  const EventSource<>& on_disposed() const { return on_disposed_; }
  void dispose();

  // FunctionHost
  Result<const Catalog*> resolve_function_catalog(const std::string* catalog_id,
                                                  const std::string& function_name) override;
  void report_function_error(const Error& error, const std::string& function_name) override;
  Status validate_function_args(const FunctionApi& api, const Json& args) override;
  bool routes_to_agent() const override;
  AgentCallStatus agent_function_value(const std::string& name, const std::string& catalog_id, const Json& args,
                                       Json* value) override;
  Status open_url(const std::string& url) override;

 private:
  std::string id_;
  CatalogPtr default_catalog_;
  CatalogMap available_catalogs_;
  std::string protocol_version_;
  bool send_data_model_;
  std::string root_id_;
  DataModel data_model_;
  SurfaceComponentsModel components_;
  Signal<Json> metadata_;
  EventSource<Json> on_metadata_changed_;
  EventSource<Json> on_action_;
  EventSource<Json> on_error_;
  EventSource<Error> on_function_error_;
  EventSource<> on_disposed_;
  void settle_agent_call(const AgentFunctionCall& call, const std::string& key, Result<Json> outcome);

  AgentFunctionCaller* agent_caller_ = nullptr;
  AgentCallCache agent_cache_;
  EventSource<AgentCallSettlement> on_agent_call_settled_;
  uint32_t agent_generation_ = 0;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
  UrlHandler url_handler_;
  std::function<double()> clock_;
  bool disposed_ = false;
};

/** Formats seconds since the epoch as an ISO 8601 UTC timestamp with milliseconds. */
std::string iso8601_utc(double seconds_since_epoch);

class SurfaceGroupModel {
 public:
  using Map = std::map<std::string, std::unique_ptr<SurfaceModel>, std::less<>>;

  SurfaceGroupModel() = default;
  ~SurfaceGroupModel();
  SurfaceGroupModel(const SurfaceGroupModel&) = delete;
  SurfaceGroupModel& operator=(const SurfaceGroupModel&) = delete;

  /** Adds a surface; ErrorKind::Integrity if its id is taken. Fires on_surface_created. */
  Status add_surface(std::unique_ptr<SurfaceModel> surface);
  /** Disposes and removes a surface; fires on_surface_deleted. No-op for unknown ids. */
  void delete_surface(const std::string& id);
  SurfaceModel* get_surface(std::string_view id) const;
  const Map& surfaces() const { return surfaces_; }

  const EventSource<SurfaceModel*>& on_surface_created() const { return on_surface_created_; }
  const EventSource<std::string>& on_surface_deleted() const { return on_surface_deleted_; }
  /** Re-emits every surface's actions. */
  const EventSource<Json>& on_action() const { return on_action_; }

  void dispose();

 private:
  Map surfaces_;
  std::map<std::string, Subscription, std::less<>> action_subs_;
  EventSource<SurfaceModel*> on_surface_created_;
  EventSource<std::string> on_surface_deleted_;
  EventSource<Json> on_action_;
};

/** Canonical protocol versions a catalog version may serve (v0.9 and v0.9.1 interoperate). */
bool is_catalog_version_compatible(std::string_view catalog_version, std::string_view message_version);

}  // namespace a2ui

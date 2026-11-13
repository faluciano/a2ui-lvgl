// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// DataContext: evaluates dynamic values (literals, {"@path"}, {"@call"}) in a
// data scope. Ported from the Swift core's DataContext (v1.0 semantics).
//
// Evaluation never aborts: a failed binding or call is reported to the
// FunctionHost and yields null, exactly as the reference SDKs do. Callers
// that need to know can inspect had_failure()/had_deferred().
//
// Reactivity: evaluation is eager. A caller that wants to re-evaluate when
// data changes passes a dependency sink and subscribes to the recorded paths.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "a2ui/catalog.h"
#include "a2ui/data_model.h"
#include "a2ui/error.h"
#include "a2ui/json.h"

namespace a2ui {

/** What a DataContext needs from its surface (or a test fixture). */
class FunctionHost {
 public:
  virtual ~FunctionHost() = default;

  /**
   * v1.0 catalog resolution for a function call: the named catalog when
   * `catalog_id` is given, else the surface default. Fails with CatalogError.
   */
  virtual Result<const Catalog*> resolve_function_catalog(const std::string* catalog_id,
                                                          const std::string& function_name) = 0;

  /** Receives every evaluation failure (the surface's error channel). */
  virtual void report_function_error(const Error& error, const std::string& function_name) = 0;

  /**
   * Validates resolved arguments against a function's args schema. The
   * default accepts everything; the schema layer overrides it.
   */
  virtual Status validate_function_args(const FunctionApi& api, const Json& args) {
    return ok_status();
  }

  /**
   * Agent routing for calls with no local implementation. Return true and
   * set `*value` when a result is available; return false while pending.
   * `routes_to_agent` false means such calls fail locally.
   */
  virtual bool routes_to_agent() const { return false; }
  virtual bool agent_function_value(const std::string& name, const std::string& catalog_id,
                                    const Json& args, Json* value) {
    return false;
  }

  /** openUrl side effect. */
  virtual Status open_url(const std::string& url) {
    return make_error(ErrorKind::Generic, "No URL handler is configured");
  }
};

class DataContext : public FunctionContext {
 public:
  DataContext(DataModel& model, FunctionHost* host, std::string path = "/",
              std::optional<long> index = std::nullopt, bool user_activated = false);

  // FunctionContext
  const std::string& scope_path() const override { return path_; }
  std::optional<long> scope_index() const override { return index_; }
  bool user_activated() const override { return user_activated_; }
  Result<Json> resolve(const Json& dynamic_value) override { return resolve_value(dynamic_value); }
  const std::string& active_catalog_id() const override { return active_catalog_id_; }
  Status open_url(const std::string& url) override;

  /** Evaluates a dynamic value. Failures are reported to the host and yield null. */
  Json resolve_value(const Json& value);

  /** Evaluates with a different user-activation state (actions). */
  Json resolve_value(const Json& value, bool user_activated);

  /** A child scope at a path relative to this one, optionally a repeater index. */
  DataContext nested(const std::string& relative_path, std::optional<long> index = std::nullopt) const;

  /** Resolves `p` against this scope. */
  std::string absolute_path(const std::string& p) const { return path::resolve(p, path_); }

  /** Writes through a (possibly relative) binding path. */
  Status set(const std::string& p, Json value);

  /** Records every absolute data path read while resolving (for re-evaluation). */
  void set_dependency_sink(std::vector<std::string>* sink) { deps_ = sink; }

  /** A call failed during evaluation since the last reset. */
  bool had_failure() const { return failed_; }
  /** A call is waiting on the agent since the last reset. */
  bool had_deferred() const { return deferred_; }
  void reset_flags() {
    failed_ = false;
    deferred_ = false;
  }

  DataModel& model() const { return *model_; }
  FunctionHost* host() const { return host_; }

  /** v1.0 reserved-key check: single-'@' keys other than @path/@call are invalid. */
  static bool is_invalid_reserved_key(const std::string& key);

 private:
  Json evaluate_call(const std::string& name, const Json& call);
  Json route_to_agent(const std::string& name, const FunctionDecl* decl, const std::string& catalog_id,
                      const Json& call);
  bool resolve_arguments(const Json& call, Json* out);
  void fail(Error error, const std::string& function_name);

  DataModel* model_;
  FunctionHost* host_;
  std::string path_;
  std::optional<long> index_;
  bool user_activated_;
  std::string active_catalog_id_;
  int running_function_depth_ = 0;
  bool failed_ = false;
  bool deferred_ = false;
  std::vector<std::string>* deps_ = nullptr;
};

namespace function_errors {
Error not_found(const std::string& name);
Error caller_disallowed(const std::string& name);
Error user_activation_required(const std::string& name);
Error invalid_arguments(const std::string& name);
}  // namespace function_errors

/** Coerces ValidationResult values to booleans where an args schema expects DynamicBoolean. */
Json coerce_validation_results(const Json& schema, const Json& value);

}  // namespace a2ui

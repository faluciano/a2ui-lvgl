// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// The processing layer (blueprint section B): version adapters that turn
// wire messages into InternalOperations, and the MessageProcessor that
// validates them against the surface they join and applies them to the
// state models. Ported from the Swift core's Processing/ (MessageProcessor,
// Adapters, InternalOperation, MessageErrorMapper), with conformance
// semantics from the Python SDK.
//
// Protocol scope: v1.0. The VersionAdapterRegistry is the seam where
// pre-1.0 adapters would be registered; none are built in.
//
// RPC: callRendererFunction and agentFunctionResponse are validated by the
// adapter and forwarded to an RpcHandler. Without one they are dropped.

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "a2ui/catalog.h"
#include "a2ui/error.h"
#include "a2ui/json.h"
#include "a2ui/surface_model.h"
#include "a2ui/validation_config.h"

namespace a2ui {

/** The version-neutral vocabulary of the processing pipeline. */
struct InternalOperation {
  enum class Type : uint8_t {
    CreateSurface,
    UpdateComponents,
    UpdateDataModel,
    DeleteSurface,
    CallRendererFunction,
    AgentFunctionResponse,
  };
  Type type = Type::CreateSurface;
  /** Normalized protocol version of the originating envelope ("1.0"). */
  std::string version;
  /** Empty when the message names none (call operations may omit it). */
  std::string surface_id;

  // createSurface
  std::optional<std::string> catalog_id;  // also callRendererFunction.callFunction.catalogId
  bool send_data_model = false;
  Json components;  // array, or undefined
  Json data_model;  // object, or undefined
  Json metadata;    // validated envelope, or undefined
  std::optional<std::string> root;  // v0.8 only
  Json theme;                       // pre-v1.0 only

  // updateDataModel
  std::string path = "/";
  Json value;

  // callRendererFunction / agentFunctionResponse
  std::string function_call_id;
  std::string call;
  Json args;
  bool user_activated = false;
  Json error;  // agentFunctionResponse.error {code, message}

  static const char* type_name(Type t);
};

struct AdapterOptions {
  /** Relaxed renderers drop fields the version does not define (createSurface.theme). */
  bool allow_unknown_elements = false;
};

class VersionAdapter {
 public:
  virtual ~VersionAdapter() = default;
  /** Normalized protocol version ("1.0"). */
  virtual const std::string& version() const = 0;
  /** Action keys this version defines. */
  virtual const std::vector<std::string>& valid_actions() const = 0;
  /** Whether a catalog's declared version may be used under this adapter. */
  virtual bool is_catalog_compatible(std::string_view catalog_version) const;
  /**
   * Validates a payload (one message, an array, or {"messages": [...]})
   * and appends its operations. Fails with ErrorKind::Validation (or
   * Recursion for nesting past the limits) without appending anything.
   */
  virtual Status extract_operations(const Json& payload, const AdapterOptions& options,
                                    std::vector<InternalOperation>* out) const = 0;
};

/** The v1.0 adapter. */
std::unique_ptr<VersionAdapter> make_v10_adapter();

/** Resolves adapters by version string or by a payload's version tag. */
class VersionAdapterRegistry {
 public:
  /** A registry holding the v1.0 adapter. */
  static std::shared_ptr<VersionAdapterRegistry> make_default();

  void register_adapter(std::unique_ptr<VersionAdapter> adapter);
  Result<const VersionAdapter*> get_adapter(std::string_view version) const;
  /** Reads the first message's `version`. */
  Result<const VersionAdapter*> resolve_from_payload(const Json& payload) const;
  /** Action keys of every registered adapter plus the legacy (pre-1.0) ones. */
  std::vector<std::string> all_known_actions() const;

 private:
  std::vector<std::unique_ptr<VersionAdapter>> adapters_;
};

class MessageProcessor;

/**
 * Receives inbound RPC operations (blueprint section D). The processor
 * resolves the target surface (null when the call names none or names an
 * unknown one) and forwards; responses go out through the processor's
 * outbound listener (MessageProcessor::send_outbound).
 */
class RpcHandler {
 public:
  virtual ~RpcHandler() = default;
  virtual void handle_call_renderer_function(const InternalOperation& op, SurfaceModel* surface,
                                             MessageProcessor& processor) = 0;
  virtual void handle_agent_function_response(const InternalOperation& op, MessageProcessor& processor) = 0;
  /** A surface was deleted: cancel its pending outbound calls. */
  virtual void cancel_surface_calls(const std::string& surface_id) {}
  virtual void dispose() {}
};

/** Receives renderer-to-agent messages ({version, error} envelopes, RPC responses). */
using OutboundListener = std::function<void(const Json& message)>;

struct MessageProcessorOptions {
  /** Validation rules; std::nullopt skips schema, composition and topology checks. */
  std::optional<ValidationConfig> validation_config = ValidationConfig::strict();
  OutboundListener outbound_listener;
  /** Defaults to VersionAdapterRegistry::make_default(). */
  std::shared_ptr<VersionAdapterRegistry> adapters;
  /** Not owned; must outlive the processor. */
  RpcHandler* rpc = nullptr;
};

/** Options for one process_messages call. */
struct ExecutionContext {
  bool is_user_activated = false;
  /** When set, replaces the processor's validation rules for this call. */
  std::optional<ValidationConfig> validation_config;
  /** When true, this call skips validation (validation_config ignored). */
  bool skip_validation = false;
};

struct CapabilitiesOptions {
  /** Protocol versions to describe; at least one ("1.0"). */
  std::vector<std::string> versions;
  bool include_inline_catalogs = false;
  /** Pre-1.0 inline component envelope; unused for v1.0. */
  std::string component_envelope_ref = "common_types.json#/$defs/ComponentCommon";
};

class MessageProcessor {
 public:
  explicit MessageProcessor(std::vector<CatalogPtr> catalogs, MessageProcessorOptions options = {});
  ~MessageProcessor();
  MessageProcessor(const MessageProcessor&) = delete;
  MessageProcessor& operator=(const MessageProcessor&) = delete;

  SurfaceGroupModel& model() { return model_; }
  const SurfaceGroupModel& model() const { return model_; }
  /** The supported catalogs (first registration of an id wins). */
  const std::vector<CatalogPtr>& catalogs() const { return catalogs_; }
  /** A supported catalog by id, or null. */
  CatalogPtr find_catalog(std::string_view id) const;

  /**
   * Validates and applies a payload: one message, an array of messages, or
   * {"messages": [...]}. The whole payload passes envelope validation before
   * any operation runs; operations then run in order and the first failure
   * stops the call. A failure is mapped to a {version, error} envelope and
   * sent to the outbound listener (and on_error()), then returned with its
   * wire code, surfaceId and path filled in.
   */
  Status process_messages(const Json& payload, const ExecutionContext& context = {});
  /** Parses JSON text (ErrorKind::Parse on malformed input), then process_messages. */
  Status process_text(std::string_view text, const ExecutionContext& context = {});

  /**
   * {"<version>": {"supportedCatalogIds": [...], "inlineCatalogs"?: [...]}}
   * for each requested version. ErrorKind::Validation when none is given.
   */
  Result<Json> get_renderer_capabilities(const CapabilitiesOptions& options) const;

  /**
   * {"version", "surfaces": {id: dataModel}} for surfaces with
   * sendDataModel, filtered to `version` when given. Null when no surface
   * qualifies; ErrorKind::Validation when `version` is empty and the
   * surfaces disagree on their protocol version.
   */
  Result<Json> get_renderer_data_model(std::string_view version = {}) const;

  void set_outbound_listener(OutboundListener listener) { outbound_ = std::move(listener); }
  /** Sends a renderer-to-agent message to the outbound listener. */
  void send_outbound(const Json& message) const;
  /** Every mapped error envelope (also sent to the outbound listener). */
  const EventSource<Json>& on_error() const { return on_error_; }

  const std::optional<ValidationConfig>& validation_config() const { return validation_config_; }

  /** Deletes every surface and disposes the RPC handler. Later calls fail with ErrorKind::State. */
  void dispose();
  bool disposed() const { return disposed_; }

 private:
  struct CallState;

  Status run_operation(const InternalOperation& op, CallState& call);
  Status create_surface(const InternalOperation& op, CallState& call);
  Status update_components(const std::string& surface_id, const Json& components, CallState& call);
  Status update_data_model(const InternalOperation& op);
  Status build_components(SurfaceModel& surface, const Json& components, CallState& call,
                          SurfaceComponentsModel::Batch* batch);
  Status validate_nested_calls(SurfaceModel& surface, const SurfaceComponentsModel::Batch& batch,
                               CallState& call, Json* details, std::string* summary, std::string* first_path);
  Error report(Error error, const Json& payload, const InternalOperation* op);

  std::vector<CatalogPtr> catalogs_;
  std::optional<ValidationConfig> validation_config_;
  OutboundListener outbound_;
  std::shared_ptr<VersionAdapterRegistry> adapters_;
  RpcHandler* rpc_;
  SurfaceGroupModel model_;
  EventSource<Json> on_error_;
  bool disposed_ = false;
};

/**
 * Maps an error to the renderer-to-agent error envelope (MessageErrorMapper):
 * {version, error: {code, surfaceId | functionCallId, path?, message}}.
 * The wire code is error.code when set, else derived from the error kind.
 */
Json map_error_to_renderer_message(const Error& error, std::string_view version);

}  // namespace a2ui

// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// RpcHandler. See a2ui/rpc.h. Port of swift/core/Sources/A2UICore/RPC/
// RPCHandler.swift, with the lifecycle of python/a2ui_core rpc_handler.py.

#include "a2ui/rpc.h"

#include <algorithm>
#include <utility>

#include "a2ui/data_context.h"
#include "a2ui/data_model.h"
#include "a2ui/message_processor.h"
#include "a2ui/schema.h"
#include "a2ui/surface_model.h"

namespace a2ui {

Error rpc_error(const char* code, std::string message, std::string function_call_id) {
  Error e = make_error(ErrorKind::Rpc, std::move(message), code);
  e.function_call_id = std::move(function_call_id);
  return e;
}

namespace {

constexpr const char* kUnresolvedArgumentMessage = "An argument of the function call could not be resolved.";
constexpr const char* kExecutionFailedMessage = "An error occurred during function execution.";

bool is_blank(const std::string& s) {
  return std::all_of(s.begin(), s.end(), [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; });
}

std::string version_label(const std::string& normalized) {
  if (normalized.empty()) return "v0.9";
  return normalized == "1.0" ? "1.0" : "v" + normalized;
}

Json apply_schema_defaults(const Json& schema, const Json& args) {
  const Json* props = schema.find("properties");
  if (!props || !props->is_object() || !args.is_object()) return args;
  Json out = args;
  for (const auto& m : props->as_object()) {
    if (Json* existing = out.find_mut(m.first)) {
      if (existing->is_object()) *existing = apply_schema_defaults(m.second, *existing);
    } else if (const Json* def = m.second.find("default")) {
      out.set(m.first, *def);
    }
  }
  return out;
}

/**
 * The FunctionHost of an agent-initiated call's arguments and execution
 * (Swift's RPCArgumentFunctionHandler): the target surface when there is
 * one, else a headless host over the processor's catalogs. It never routes
 * to the agent, and keeps the first evaluation error for the response.
 */
class InboundCallHost : public FunctionHost {
 public:
  InboundCallHost(SurfaceModel* surface, const Catalog* matched, const std::vector<CatalogPtr>& catalogs)
      : surface_(surface), matched_(matched), catalogs_(catalogs) {}

  const Error* captured() const { return captured_ ? &error_ : nullptr; }

  Result<const Catalog*> resolve_function_catalog(const std::string* catalog_id,
                                                  const std::string& function_name) override {
    if (surface_) {
      auto r = surface_->resolve_function_catalog(catalog_id, function_name);
      if (r.ok()) return r;
      if (!catalog_id) return r;
      if (*catalog_id == matched_->id()) return matched_;
      if (const Catalog* c = find(*catalog_id)) return c;
      return r;
    }
    if (!catalog_id) return matched_;
    if (const Catalog* c = find(*catalog_id)) return c;
    return make_error(ErrorKind::Catalog, "Catalog not found: " + *catalog_id);
  }

  void report_function_error(const Error& error, const std::string& function_name) override {
    if (!captured_) {
      captured_ = true;
      error_ = error;
    }
    if (surface_) surface_->report_function_error(error, function_name);
  }

  Status validate_function_args(const FunctionApi& api, const Json& args) override {
    if (surface_) {
      // A function of a processor catalog the surface does not hold is
      // validated against its own catalog below.
      for (const auto& entry : surface_->available_catalogs()) {
        const FunctionDecl* d = entry.second->find_function(api.name);
        if (d && &d->api == &api) return surface_->validate_function_args(api, args);
      }
    }
    return validate_against_catalog(api, args);
  }

  Status open_url(const std::string& url) override {
    return surface_ ? surface_->open_url(url) : ok_status();
  }

  /** Validates `args` against `api.args_schema` with the documents of the catalog declaring it. */
  Status validate_against_catalog(const FunctionApi& api, const Json& args) const {
    const Json& schema = api.args_schema;
    if (!schema.is_object() || !(schema.contains("type") || schema.contains("properties"))) return ok_status();
    auto validate_in = [&](const Catalog& cat) -> std::optional<Status> {
      const FunctionDecl* d = cat.find_function(api.name);
      if (!d || &d->api != &api) return std::nullopt;
      if (!cat.is_v1()) return ok_status();
      SchemaRegistry reg = cat.schema_registry();
      return validate_schema(reg, "catalog.json", schema, args);
    };
    if (auto s = validate_in(*matched_)) return *s;
    for (const auto& c : catalogs_) {
      if (auto s = validate_in(*c)) return *s;
    }
    return ok_status();
  }

 private:
  const Catalog* find(const std::string& id) const {
    for (const auto& c : catalogs_) {
      if (c->id() == id) return c.get();
    }
    return nullptr;
  }

  SurfaceModel* surface_;
  const Catalog* matched_;
  const std::vector<CatalogPtr>& catalogs_;
  bool captured_ = false;
  Error error_;
};

}  // namespace

RpcHandler::RpcHandler(std::vector<CatalogPtr> catalogs, RpcOptions options)
    : catalogs_(std::move(catalogs)), options_(std::move(options)) {}

// Destruction drops pending calls without calling back: their callbacks may
// capture objects that are already gone. dispose() fails them with CANCELLED.
RpcHandler::~RpcHandler() = default;

uint64_t RpcHandler::now_ms() const {
  if (options_.clock) return options_.clock();
  return last_tick_ms_;
}

std::string RpcHandler::next_call_id() {
  std::string id;
  do {
    id = "call-" + std::to_string(++id_counter_);
  } while (pending_.count(id));
  return id;
}

void RpcHandler::send(const Json& message) const {
  if (outbound_) {
    // The listener may dispose the handler or replace itself; call a copy.
    OutboundListener listener = outbound_;
    listener(message);
  }
}

Result<std::string> RpcHandler::call_agent_function(const std::string& surface_id, const Json& call_function,
                                                    AgentCallDone done, const AgentCallOptions& options) {
  if (disposed_) return rpc_error(rpc_code::kDisposed, "The RPC handler has been disposed.");
  if (!outbound_) {
    return rpc_error(rpc_code::kNoListener, "Cannot call an agent function without an outbound listener.");
  }
  const Json& name = call_function["@call"];
  if (!call_function.is_object() || !name.is_string() || name.as_string().empty()) {
    return rpc_error(rpc_code::kInvalidFunctionCall, "The function call has no '@call' name.");
  }
  std::string id = options.function_call_id.empty() ? next_call_id() : options.function_call_id;
  if (pending_.count(id)) {
    return rpc_error(rpc_code::kDuplicate, "A call with functionCallId '" + id + "' is already pending.", id);
  }

  Pending p;
  p.surface_id = surface_id;
  p.name = name.as_string();
  p.done = std::move(done);
  p.seq = ++seq_;
  p.timeout_ms = options.timeout_ms ? *options.timeout_ms : options_.default_timeout_ms;
  if (options_.clock || ticked_) {
    p.started = true;
    p.start_ms = now_ms();
  }
  pending_.emplace(id, std::move(p));

  Json body = Json::object({{"surfaceId", Json(surface_id)}, {"functionCallId", Json(id)}});
  body.set("callFunction", call_function);
  // Registered before sending: a listener may answer synchronously.
  send(Json::object({{"version", Json(options.version.empty() ? std::string("1.0") : options.version)},
                     {"callAgentFunction", std::move(body)}}));
  return id;
}

Result<std::string> RpcHandler::call_agent(const std::string& surface_id, const AgentFunctionCall& call,
                                           AgentCallDone done) {
  return call_agent_function(surface_id, call.to_call_function(), std::move(done));
}

void RpcHandler::reject_where(const std::function<bool(const std::string&, const Pending&)>& pred,
                              const std::function<Error(const std::string&, const Pending&)>& make) {
  std::vector<std::pair<std::string, Pending>> hit;
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (pred(it->first, it->second)) {
      hit.emplace_back(it->first, std::move(it->second));
      it = pending_.erase(it);
    } else {
      ++it;
    }
  }
  std::sort(hit.begin(), hit.end(), [](const auto& a, const auto& b) { return a.second.seq < b.second.seq; });
  // Callbacks run after the registry is consistent: they may make new calls.
  for (auto& h : hit) {
    if (h.second.done) h.second.done(make(h.first, h.second));
  }
}

void RpcHandler::tick(uint64_t now) {
  ticked_ = true;
  last_tick_ms_ = now;
  for (auto& p : pending_) {
    if (!p.second.started) {
      p.second.started = true;
      p.second.start_ms = now;
    }
  }
  reject_where(
      [now](const std::string&, const Pending& p) {
        return p.timeout_ms > 0 && now >= p.start_ms && now - p.start_ms >= p.timeout_ms;
      },
      [](const std::string& id, const Pending& p) {
        return rpc_error(rpc_code::kTimeout,
                         "Agent function call '" + p.name + "' timed out after " + std::to_string(p.timeout_ms) +
                             " ms.",
                         id);
      });
}

void RpcHandler::cancel_surface_calls(const std::string& surface_id) {
  reject_where([&surface_id](const std::string&, const Pending& p) { return p.surface_id == surface_id; },
               [&surface_id](const std::string& id, const Pending&) {
                 return rpc_error(rpc_code::kCancelled,
                                  "Pending calls for surface '" + surface_id + "' were cancelled.", id);
               });
}

void RpcHandler::cancel_all_calls() {
  reject_where([](const std::string&, const Pending&) { return true; },
               [](const std::string& id, const Pending&) {
                 return rpc_error(rpc_code::kCancelled, "Call '" + id + "' was cancelled.", id);
               });
}

void RpcHandler::dispose(const std::string& reason) {
  if (disposed_) return;
  disposed_ = true;
  outbound_ = nullptr;
  reject_where([](const std::string&, const Pending&) { return true; },
               [&reason](const std::string& id, const Pending&) {
                 return rpc_error(rpc_code::kCancelled,
                                  "The RPC handler was disposed while call '" + id + "' was pending: " + reason, id);
               });
}

bool RpcHandler::handle_agent_function_response(const InternalOperation& op) {
  auto it = pending_.find(op.function_call_id);
  if (it == pending_.end()) return false;
  Pending p = std::move(it->second);
  pending_.erase(it);
  if (!p.done) return true;
  if (op.error.is_object()) {
    const std::string code = op.error["code"].is_string() ? op.error["code"].as_string() : "UNKNOWN_ERROR";
    const std::string msg =
        op.error["message"].is_string() ? op.error["message"].as_string() : "Agent function execution failed";
    Error e = make_error(ErrorKind::Rpc, "Agent function error [" + code + "]: " + msg, code);
    e.function_call_id = op.function_call_id;
    p.done(std::move(e));
  } else {
    p.done(op.value.is_undefined() ? Json(nullptr) : op.value);
  }
  return true;
}

Json RpcHandler::handle_call_renderer_function(const InternalOperation& op, SurfaceModel* surface,
                                               bool user_activated) {
  const std::string version = op.version.empty() ? std::string("1.0") : op.version;
  const std::string& call_id = op.function_call_id;
  const std::string& name = op.call;

  auto respond = [&](Json body_value) {
    Json body = Json::object({{"functionCallId", Json(call_id)}});
    for (auto& m : body_value.as_object_mut()) body.set(m.first, std::move(m.second));
    Json reply = Json::object({{"version", Json(version)}, {"rendererFunctionResponse", std::move(body)}});
    send(reply);
    return reply;
  };
  auto fail = [&](const char* code, const std::string& message) {
    return respond(
        Json::object({{"error", Json::object({{"code", Json(code)}, {"message", Json(message)}})}}));
  };

  if (disposed_) return fail(rpc_code::kDisposed, "The renderer's RPC state was disposed.");
  if (!op.surface_id.empty() && !surface) {
    return fail(rpc_code::kInvalidFunctionCall, "Surface not found: " + op.surface_id);
  }

  // The call's catalogId, in the surface's catalogs, then the processor's.
  // Never the surface default: the schema requires catalogId.
  const std::string catalog_id = op.catalog_id ? *op.catalog_id : std::string();
  CatalogPtr catalog;
  if (surface) {
    auto it = surface->available_catalogs().find(catalog_id);
    if (it != surface->available_catalogs().end()) catalog = it->second;
  }
  if (!catalog) {
    for (const auto& c : catalogs_) {
      if (c->id() == catalog_id) {
        catalog = c;
        break;
      }
    }
  }
  if (!catalog) return fail(rpc_code::kInvalidFunctionCall, "Catalog not found: " + catalog_id);
  if (!is_catalog_version_compatible(catalog->protocol_version(), version) ||
      (surface && !surface->protocol_version().empty() &&
       !is_catalog_version_compatible(catalog->protocol_version(), surface->protocol_version()))) {
    const std::string declared = catalog->declared_protocol_version().empty()
                                     ? version_label(catalog->protocol_version())
                                     : catalog->declared_protocol_version();
    return fail(rpc_code::kInvalidFunctionCall, "Catalog '" + catalog->id() + "' protocol version (" + declared +
                                                    ") does not match message protocol version.");
  }

  // A schema-only declaration has no renderer implementation to run.
  const FunctionDecl* decl = catalog->find_function(name);
  if (!decl || !decl->has_local_impl()) {
    return fail(rpc_code::kInvalidFunctionCall, "Function not found: " + name);
  }
  if (decl->api.allowed_callers == AllowedCallers::RendererOnly) {
    return fail(rpc_code::kInvalidFunctionCall,
                "Function '" + name + "' cannot be called by agent (allowedCallers is rendererOnly).");
  }
  if (decl->api.requires_user_activation && !user_activated) {
    return fail(rpc_code::kInvalidFunctionCall,
                "Function '" + name + "' requires user activation context to execute.");
  }

  // Arguments resolve against the surface's root scope, or an isolated empty
  // data model. They never route to the agent: a nested call the renderer
  // cannot run fails here.
  InboundCallHost host(surface, catalog.get(), catalogs_);
  DataModel headless_model;
  DataModel& model = surface ? surface->data_model() : headless_model;
  DataContext ctx(model, &host, "/", std::nullopt, user_activated);
  ctx.set_active_catalog_id(catalog->id());

  Json args = Json::object();
  for (const auto& m : op.args.as_object()) args.set(m.first, ctx.resolve_value(m.second));
  if (const Error* e = host.captured()) {
    return fail(rpc_code::kExecutionError, is_blank(e->message) ? kUnresolvedArgumentMessage : e->message);
  }
  if (!host.validate_function_args(decl->api, args).ok()) {
    return fail(rpc_code::kInvalidFunctionCall, "Invalid arguments for function '" + name + "'.");
  }
  args = apply_schema_defaults(decl->api.args_schema, args);

  auto result = decl->impl(args, ctx);
  if (!result.ok()) {
    const std::string& msg = result.error().message;
    return fail(rpc_code::kExecutionError, is_blank(msg) ? kExecutionFailedMessage : msg);
  }
  Json value = std::move(result).value();
  return respond(Json::object({{"value", value.is_undefined() ? Json(nullptr) : std::move(value)}}));
}

}  // namespace a2ui

// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Version adapters: payload unwrapping, the one-action rule, the version
// header, nesting limits and per-action envelope checks (Swift
// Processing/Adapters/{BaseVersionAdapter,V10VersionAdapter,
// VersionAdapterFactory}.swift; messages follow the Python SDK).
//
// Errors carry `path` as a JSON Pointer relative to the action payload
// ("/surfaceId") and `details` [{path, code, message}].

#include <algorithm>
#include <utility>

#include "a2ui/message_processor.h"
#include "a2ui/metadata.h"
#include "a2ui/schema.h"

namespace a2ui {
namespace {

Error envelope_error(std::string message, std::string pointer = {}, const char* detail_code = nullptr,
                     std::string detail_message = {}) {
  Error e = make_error(ErrorKind::Validation, std::move(message));
  e.path = pointer;
  if (detail_code) {
    e.details = Json::array({Json::object({{"path", Json(pointer)},
                                           {"code", Json(detail_code)},
                                           {"message", Json(detail_message.empty() ? e.message : detail_message)}})});
  }
  return e;
}

std::string join(const std::vector<std::string>& v, const char* sep = ", ") {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += sep;
    out += v[i];
  }
  return out;
}

// RFC 6901 pointer syntax as the other SDKs relax it: every '~' escapes 0 or 1.
bool valid_pointer_syntax(const std::string& p) {
  for (size_t i = 0; i < p.size(); ++i) {
    if (p[i] == '~' && (i + 1 >= p.size() || (p[i + 1] != '0' && p[i + 1] != '1'))) return false;
  }
  return true;
}

// Nesting limits and binding path syntax over one message. Raw data
// (dataModel, value, metadata of the action) is not checked for
// expressions. Iterative: payloads are untrusted and stacks are small.
Status check_paths_and_recursion(const Json& message) {
  struct Frame {
    const Json* value;
    int depth;
    int fn_depth;
    bool expressions;
  };
  std::vector<Frame> stack{{&message, 0, 0, true}};
  while (!stack.empty()) {
    Frame f = stack.back();
    stack.pop_back();
    if (f.depth > kMaxGlobalDepth) {
      return make_error(ErrorKind::Recursion,
                        "Global recursion limit exceeded: Depth > " + std::to_string(kMaxGlobalDepth));
    }
    if (f.expressions && f.fn_depth > kMaxFunctionCallDepth) {
      return make_error(ErrorKind::Recursion,
                        "Recursion limit exceeded: functionCall depth > " + std::to_string(kMaxFunctionCallDepth));
    }
    const Json& v = *f.value;
    if (v.is_object()) {
      int next_fn = f.fn_depth;
      if (f.expressions) {
        for (const char* key : {"@path", "path"}) {
          const Json& p = v[key];
          if (p.is_string() && !valid_pointer_syntax(p.as_string())) {
            const std::string msg = "Invalid path syntax: '" + p.as_string() + "'";
            return envelope_error(msg, std::string("/") + key, "invalid_pointer");
          }
        }
        if (v.contains("@call") || v.contains("call")) ++next_fn;
      }
      for (const auto& m : v.as_object()) {
        const bool raw = f.depth == 1 && (m.first == "dataModel" || m.first == "value" || m.first == "metadata");
        stack.push_back({&m.second, f.depth + 1, next_fn, f.expressions && !raw});
      }
    } else if (v.is_array()) {
      for (const auto& e : v.as_array()) stack.push_back({&e, f.depth + 1, f.fn_depth, f.expressions});
    }
  }
  return ok_status();
}

class V10Adapter final : public VersionAdapter {
 public:
  V10Adapter()
      : version_("1.0"),
        actions_{"agentFunctionResponse", "callRendererFunction", "createSurface",
                 "deleteSurface",         "updateComponents",     "updateDataModel"} {}

  const std::string& version() const override { return version_; }
  const std::vector<std::string>& valid_actions() const override { return actions_; }

  Status extract_operations(const Json& payload, const AdapterOptions& options,
                            std::vector<InternalOperation>* out) const override {
    std::vector<InternalOperation> ops;
    A2UI_RETURN_IF_ERROR(extract(payload, options, &ops, 0));
    for (auto& op : ops) out->push_back(std::move(op));
    return ok_status();
  }

  void set_known_actions(std::vector<std::string> known) { known_ = std::move(known); }

 private:
  bool is_action(const std::string& k) const { return std::find(actions_.begin(), actions_.end(), k) != actions_.end(); }

  Status extract(const Json& payload, const AdapterOptions& options, std::vector<InternalOperation>* out,
                 int depth) const {
    if (payload.is_nullish()) return ok_status();
    if (payload.is_array()) {
      // The one-action rule is checked for every message before any is extracted.
      for (size_t i = 0; i < payload.size(); ++i) {
        if (!payload[i].is_object()) {
          return envelope_error("Message at index " + std::to_string(i) + " must be an object", "", "type_mismatch",
                                "Message must be an object");
        }
        std::string action;
        A2UI_RETURN_IF_ERROR(single_action(payload[i], &action));
      }
      for (const auto& item : payload.as_array()) A2UI_RETURN_IF_ERROR(extract_message(item, options, out));
      return ok_status();
    }
    if (payload.is_object()) {
      if (const Json* msgs = payload.find("messages"); msgs && msgs->is_array() && depth == 0) {
        return extract(*msgs, options, out, depth + 1);
      }
      return extract_message(payload, options, out);
    }
    return envelope_error("Payload must be a JSON object or array of objects", "", "type_mismatch",
                          "Expected object or array");
  }

  Status single_action(const Json& msg, std::string* action) const {
    std::vector<std::string> present;
    for (const auto& a : actions_) {
      const Json* body = msg.find(a);
      if (!body) continue;
      present.push_back(a);
      if (body->is_object()) {
        if (const Json* sid = body->find("surfaceId"); sid && !sid->is_string()) {
          return envelope_error("surfaceId must be a string", "/surfaceId", "type_mismatch");
        }
      }
    }
    if (present.size() > 1) {
      return envelope_error("Message contains multiple conflicting update actions: " + join(present) + ".", "",
                            "invalid_value", "Message must contain exactly one action key");
    }
    if (present.empty()) {
      for (const auto& m : msg.as_object()) {
        if (std::find(known_.begin(), known_.end(), m.first) != known_.end()) {
          return envelope_error("Invalid 1.0 message: action '" + m.first +
                                    "' is not supported in protocol version 1.0. Allowed actions: " + join(actions_) +
                                    ".",
                                "/version", "invalid_value");
        }
      }
      return envelope_error("Invalid 1.0 message: message must contain exactly one update action: " +
                                join(actions_) + ".",
                            "", "missing_field", "Message must contain an action key");
    }
    for (const auto& m : msg.as_object()) {
      if (m.first != "version" && m.first != present[0]) {
        return envelope_error("Invalid 1.0 message: unrecognized property '" + m.first + "'", "/" + m.first,
                              "invalid_value");
      }
    }
    *action = present[0];
    return ok_status();
  }

  Status extract_message(const Json& msg, const AdapterOptions& options, std::vector<InternalOperation>* out) const {
    if (!msg.is_object()) {
      return envelope_error("Message must be an object", "", "type_mismatch");
    }
    std::string action;
    A2UI_RETURN_IF_ERROR(single_action(msg, &action));
    const Json& body = msg[action];
    if (!body.is_object()) {
      return envelope_error("Payload for action '" + action + "' must be an object", "", "type_mismatch",
                            "Action payload must be an object");
    }
    const Json* ver = msg.find("version");
    if (!ver) {
      return envelope_error("Invalid 1.0 message: version: 'version' is a required property", "/version",
                            "missing_field", "'version' is a required property");
    }
    if (!ver->is_string() || normalize_protocol_version(ver->as_string()) != "1.0") {
      return envelope_error("Invalid 1.0 message: version: Input should be '1.0'", "/version", "invalid_value",
                            "Input should be '1.0'");
    }
    Status depth = check_paths_and_recursion(msg);
    if (!depth.ok()) {
      Error e = depth.take_error();
      if (body["surfaceId"].is_string()) e.surface_id = body["surfaceId"].as_string();
      return e;
    }

    InternalOperation op;
    op.version = version_;
    Status s = ok_status();
    if (action == "createSurface") {
      s = create_surface(body, options, &op);
    } else if (action == "updateComponents") {
      s = update_components(body, &op);
    } else if (action == "updateDataModel") {
      s = update_data_model(body, &op);
    } else if (action == "deleteSurface") {
      s = keys(body, action, {"surfaceId"});
      if (s.ok()) s = require_surface_id(body, action, &op);
      op.type = InternalOperation::Type::DeleteSurface;
    } else if (action == "callRendererFunction") {
      s = call_renderer_function(body, &op);
    } else {
      s = agent_function_response(body, &op);
    }
    if (!s.ok()) {
      Error e = s.take_error();
      if (e.surface_id.empty() && body["surfaceId"].is_string()) e.surface_id = body["surfaceId"].as_string();
      if (action == "callRendererFunction" || action == "agentFunctionResponse") {
        if (action == "callRendererFunction") e.code = "INVALID_FUNCTION_CALL";
        if (body["functionCallId"].is_string()) e.function_call_id = body["functionCallId"].as_string();
      }
      return e;
    }
    out->push_back(std::move(op));
    return ok_status();
  }

  static Status keys(const Json& body, const std::string& action, std::initializer_list<const char*> allowed) {
    for (const auto& m : body.as_object()) {
      bool ok = false;
      for (const char* a : allowed) ok = ok || m.first == a;
      if (!ok) {
        return envelope_error("Invalid 1.0 message: " + action + ": Additional properties are not allowed ('" +
                                  m.first + "' was unexpected)",
                              "/" + escape_pointer_token(m.first), "extra_field",
                              "Additional properties are not allowed ('" + m.first + "' was unexpected)");
      }
    }
    return ok_status();
  }

  static Status require_surface_id(const Json& body, const std::string& action, InternalOperation* op) {
    const Json* sid = body.find("surfaceId");
    if (!sid) {
      return envelope_error("Invalid 1.0 message: " + action + ".surfaceId: 'surfaceId' is a required property",
                            "/surfaceId", "missing_field", "'surfaceId' is a required property");
    }
    if (!sid->is_string() || sid->as_string().empty()) {
      return envelope_error("surfaceId must be a non-empty string", "/surfaceId",
                            sid->is_string() ? "invalid_value" : "type_mismatch");
    }
    op->surface_id = sid->as_string();
    return ok_status();
  }

  static Status components_array(const Json& v, const std::string& action) {
    if (!v.is_array()) {
      return envelope_error("Invalid 1.0 message: " + action + ".components must be an array", "/components",
                            "type_mismatch", "Components must be an array");
    }
    if (v.empty()) {
      return envelope_error("Invalid 1.0 message: " + action + ".components must contain at least 1 item",
                            "/components", "invalid_value", "Components array must contain at least 1 item");
    }
    for (size_t i = 0; i < v.size(); ++i) {
      const std::string at = "/components/" + std::to_string(i);
      const Json& c = v[i];
      if (!c.is_object()) {
        return envelope_error("Invalid 1.0 message: each component in " + action + ".components must be an object",
                              at, "type_mismatch", "Component definition must be an object");
      }
      if (c["component"].is_string() && c["component"].as_string() == "Surface") {
        return envelope_error(
            "Component type cannot be \"Surface\". \"Surface\" is a top-level protocol container defined in "
            "createSurface, not a child component.",
            at + "/component", "invalid_value");
      }
      if (const Json* md = c.find("@metadata")) {
        if (md->is_null()) {
          return envelope_error("'@metadata' must be an object", at + "/@metadata", "type_mismatch");
        }
        Status s = validate_metadata(*md, "component metadata");
        if (!s.ok()) return envelope_error(s.error().message, at + "/@metadata", "invalid_metadata");
      }
    }
    return ok_status();
  }

  Status create_surface(const Json& body_in, const AdapterOptions& options, InternalOperation* op) const {
    op->type = InternalOperation::Type::CreateSurface;
    const Json* body = &body_in;
    Json dropped;
    if (options.allow_unknown_elements && body_in.contains("theme")) {
      dropped = body_in;  // v1.0 removed `theme`; relaxed renderers drop it.
      dropped.erase("theme");
      body = &dropped;
    }
    A2UI_RETURN_IF_ERROR(keys(*body, "createSurface",
                              {"surfaceId", "catalogId", "sendDataModel", "components", "dataModel", "metadata"}));
    A2UI_RETURN_IF_ERROR(require_surface_id(*body, "createSurface", op));
    if (const Json* c = body->find("catalogId"); c && !c->is_null()) {
      if (!c->is_string() || c->as_string().empty()) {
        return envelope_error("Invalid 1.0 message: createSurface.catalogId must be a non-empty string", "/catalogId",
                              c->is_string() ? "invalid_value" : "type_mismatch", "Field 'catalogId' must be a string");
      }
      op->catalog_id = c->as_string();
    }
    if (const Json* s = body->find("sendDataModel"); s && !s->is_null()) {
      if (!s->is_bool()) {
        return envelope_error("Invalid 1.0 message: createSurface.sendDataModel must be a boolean", "/sendDataModel",
                              "type_mismatch", "Field 'sendDataModel' must be a boolean");
      }
      op->send_data_model = s->as_bool();
    }
    if (const Json* c = body->find("components")) {
      A2UI_RETURN_IF_ERROR(components_array(*c, "createSurface"));
      op->components = *c;
    }
    if (const Json* d = body->find("dataModel"); d && !d->is_null()) {
      if (!d->is_object()) {
        return envelope_error("Invalid 1.0 message: createSurface.dataModel must be an object", "/dataModel",
                              "type_mismatch", "Field 'dataModel' must be an object");
      }
      op->data_model = *d;
    }
    if (const Json* m = body->find("metadata")) {
      if (m->is_null()) {
        return envelope_error("Invalid 1.0 message: createSurface.metadata must be an object", "/metadata",
                              "type_mismatch");
      }
      Status s = validate_metadata(*m, "surface '" + op->surface_id + "' metadata");
      if (!s.ok()) return envelope_error(s.error().message, "/metadata", "invalid_metadata");
      op->metadata = *m;
    }
    return ok_status();
  }

  static Status update_components(const Json& body, InternalOperation* op) {
    op->type = InternalOperation::Type::UpdateComponents;
    A2UI_RETURN_IF_ERROR(keys(body, "updateComponents", {"surfaceId", "components"}));
    A2UI_RETURN_IF_ERROR(require_surface_id(body, "updateComponents", op));
    const Json* c = body.find("components");
    if (!c) {
      return envelope_error("Invalid 1.0 message: updateComponents.components: 'components' is a required property",
                            "/components", "missing_field", "'components' is a required property");
    }
    A2UI_RETURN_IF_ERROR(components_array(*c, "updateComponents"));
    op->components = *c;
    return ok_status();
  }

  static Status update_data_model(const Json& body, InternalOperation* op) {
    op->type = InternalOperation::Type::UpdateDataModel;
    A2UI_RETURN_IF_ERROR(keys(body, "updateDataModel", {"surfaceId", "path", "value"}));
    A2UI_RETURN_IF_ERROR(require_surface_id(body, "updateDataModel", op));
    if (const Json* p = body.find("path"); p && !p->is_null()) {
      if (!p->is_string()) {
        return envelope_error("Invalid 1.0 message: updateDataModel.path must be a string", "/path", "type_mismatch",
                              "Field 'path' must be a string");
      }
      op->path = p->as_string().empty() ? std::string("/") : p->as_string();
    }
    const Json* v = body.find("value");
    if (!v) {
      return envelope_error("Invalid 1.0 message: updateDataModel.value: 'value' is a required property", "/value",
                            "missing_field", "'value' is a required property");
    }
    op->value = *v;
    return ok_status();
  }

  static Status call_renderer_function(const Json& body, InternalOperation* op) {
    op->type = InternalOperation::Type::CallRendererFunction;
    A2UI_RETURN_IF_ERROR(keys(body, "callRendererFunction", {"surfaceId", "functionCallId", "callFunction"}));
    if (const Json* sid = body.find("surfaceId")) {
      if (!sid->is_string()) {
        return envelope_error("Invalid 1.0 message: callRendererFunction.surfaceId must be a string", "/surfaceId",
                              "type_mismatch");
      }
      op->surface_id = sid->as_string();
    }
    const Json* id = body.find("functionCallId");
    if (!id || !id->is_string() || id->as_string().empty()) {
      return envelope_error("Invalid 1.0 message: callRendererFunction.functionCallId is required",
                            "/functionCallId", id ? "type_mismatch" : "missing_field");
    }
    op->function_call_id = id->as_string();
    const Json* cf = body.find("callFunction");
    if (!cf || !cf->is_object()) {
      return envelope_error("Invalid 1.0 message: callRendererFunction.callFunction: 'callFunction' is a required "
                            "property",
                            "/callFunction", cf ? "type_mismatch" : "missing_field");
    }
    const Json& call = (*cf)["@call"];
    if (!call.is_string() || call.as_string().empty()) {
      return envelope_error("Invalid 1.0 message: callRendererFunction.callFunction.@call is required",
                            "/callFunction/@call", "missing_field");
    }
    for (const auto& m : cf->as_object()) {
      if (m.first != "@call" && m.first != "catalogId" && m.first != "args") {
        return envelope_error("Invalid 1.0 message: callRendererFunction.callFunction: unrecognized property '" +
                                  m.first + "'",
                              "/callFunction/" + escape_pointer_token(m.first), "extra_field");
      }
    }
    const Json* cat = cf->find("catalogId");
    if (!cat || !cat->is_string() || cat->as_string().empty()) {
      return envelope_error("Invalid 1.0 message: callRendererFunction.callFunction.catalogId is required",
                            "/callFunction/catalogId",
                            !cat ? "missing_field" : (cat->is_string() ? "invalid_value" : "type_mismatch"));
    }
    if (const Json* args = cf->find("args"); args && !args->is_null()) {
      if (!args->is_object()) {
        return envelope_error("Invalid 1.0 message: callRendererFunction.callFunction.args must be an object",
                              "/callFunction/args", "type_mismatch");
      }
      op->args = *args;
    }
    op->call = call.as_string();
    op->catalog_id = cat->as_string();
    return ok_status();
  }

  static Status agent_function_response(const Json& body, InternalOperation* op) {
    op->type = InternalOperation::Type::AgentFunctionResponse;
    A2UI_RETURN_IF_ERROR(keys(body, "agentFunctionResponse", {"functionCallId", "value", "error"}));
    const Json* id = body.find("functionCallId");
    if (!id || !id->is_string() || id->as_string().empty()) {
      return envelope_error("Invalid 1.0 message: agentFunctionResponse.functionCallId is required",
                            "/functionCallId", id ? "type_mismatch" : "missing_field");
    }
    op->function_call_id = id->as_string();
    const Json* value = body.find("value");
    const Json* error = body.find("error");
    if (!value && !error) {
      return envelope_error("Invalid 1.0 message: agentFunctionResponse must contain either 'value' or 'error'", "",
                            "missing_field");
    }
    if (value && error) {
      return envelope_error("Invalid 1.0 message: agentFunctionResponse cannot contain both 'value' and 'error'", "",
                            "invalid_value");
    }
    if (error) {
      bool ok = error->is_object() && (*error)["code"].is_string() && (*error)["message"].is_string();
      for (const auto& m : error->as_object()) ok = ok && (m.first == "code" || m.first == "message");
      if (!ok) {
        return envelope_error("Invalid 1.0 message: agentFunctionResponse.error must be an object with 'code' and "
                              "'message'",
                              "/error", error->is_object() ? "missing_field" : "type_mismatch");
      }
      op->error = *error;
    } else {
      op->value = *value;
    }
    return ok_status();
  }

  std::string version_;
  std::vector<std::string> actions_;
  std::vector<std::string> known_;
};

const char* const kLegacyActions[] = {"beginRendering", "surfaceUpdate", "dataModelUpdate"};

}  // namespace

const char* InternalOperation::type_name(Type t) {
  switch (t) {
    case Type::CreateSurface: return "createSurface";
    case Type::UpdateComponents: return "updateComponents";
    case Type::UpdateDataModel: return "updateDataModel";
    case Type::DeleteSurface: return "deleteSurface";
    case Type::CallRendererFunction: return "callRendererFunction";
    case Type::AgentFunctionResponse: return "agentFunctionResponse";
  }
  return "";
}

bool VersionAdapter::is_catalog_compatible(std::string_view catalog_version) const {
  return is_catalog_version_compatible(catalog_version, version());
}

std::unique_ptr<VersionAdapter> make_v10_adapter() {
  auto a = std::make_unique<V10Adapter>();
  std::vector<std::string> known(std::begin(kLegacyActions), std::end(kLegacyActions));
  a->set_known_actions(std::move(known));
  return a;
}

std::shared_ptr<VersionAdapterRegistry> VersionAdapterRegistry::make_default() {
  auto r = std::make_shared<VersionAdapterRegistry>();
  r->register_adapter(make_v10_adapter());
  return r;
}

void VersionAdapterRegistry::register_adapter(std::unique_ptr<VersionAdapter> adapter) {
  for (auto& a : adapters_) {
    if (a->version() == adapter->version()) {
      a = std::move(adapter);
      return;
    }
  }
  adapters_.push_back(std::move(adapter));
}

Result<const VersionAdapter*> VersionAdapterRegistry::get_adapter(std::string_view version) const {
  const std::string canonical = normalize_protocol_version(version);
  for (const auto& a : adapters_) {
    if (!canonical.empty() && a->version() == canonical) return static_cast<const VersionAdapter*>(a.get());
  }
  std::vector<std::string> supported;
  for (const auto& a : adapters_) supported.push_back(a->version() == "1.0" ? "1.0" : "v" + a->version());
  std::sort(supported.begin(), supported.end());
  Error e = envelope_error("[VersionAdapterFactory] Unsupported protocol version '" + std::string(version) +
                               "'. Supported versions: " + join(supported) + ".",
                           "/version", "invalid_value", "Unsupported protocol version '" + std::string(version) + "'");
  return e;
}

Result<const VersionAdapter*> VersionAdapterRegistry::resolve_from_payload(const Json& payload) const {
  const Json* item = &payload;
  if (payload.is_object()) {
    if (const Json* msgs = payload.find("messages"); msgs && msgs->is_array()) item = msgs;
  }
  if (item->is_array()) {
    if (item->empty()) return get_adapter("1.0");
    item = &(*item)[size_t{0}];
  } else if (item->is_nullish()) {
    return get_adapter("1.0");
  }
  if (!item->is_object()) {
    return Error(envelope_error(
        "[VersionAdapterFactory] Message payload is missing a valid 'version' string: message item at index 0 is "
        "not an object.",
        "/version", "invalid_payload_type", "Message item is not an object"));
  }
  if (const Json* v = item->find("version")) {
    if (!v->is_string()) {
      return Error(envelope_error(
          "[VersionAdapterFactory] Message payload is missing a valid 'version' string: 'version' property must be "
          "a string.",
          "/version", "type_mismatch", "Expected string for 'version'"));
    }
    return get_adapter(v->as_string());
  }
  for (const char* legacy : kLegacyActions) {
    if (item->contains(legacy)) return get_adapter("v0.8");
  }
  return Error(envelope_error(
      "[VersionAdapterFactory] Message payload is missing a valid 'version' string: no version header or legacy "
      "v0.8 action found.",
      "/version", "missing_field", "Missing required version field"));
}

std::vector<std::string> VersionAdapterRegistry::all_known_actions() const {
  std::vector<std::string> out(std::begin(kLegacyActions), std::end(kLegacyActions));
  for (const auto& a : adapters_) {
    for (const auto& act : a->valid_actions()) {
      if (std::find(out.begin(), out.end(), act) == out.end()) out.push_back(act);
    }
  }
  return out;
}

}  // namespace a2ui

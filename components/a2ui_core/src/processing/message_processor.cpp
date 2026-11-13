// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// MessageProcessor: applies InternalOperations to the state models after
// validating each against the surface it joins (Swift
// Processing/MessageProcessor.swift; ordering and messages follow the
// Python SDK's processing/message_processor.py).

#include "a2ui/message_processor.h"

#include <algorithm>
#include <set>
#include <utility>

#include "a2ui/identifier.h"
#include "a2ui/metadata.h"
#include "a2ui/schema.h"
#include "a2ui/validation.h"

namespace a2ui {
namespace {

std::string version_label(std::string_view canonical) {
  if (canonical.empty() || canonical == "1.0") return "1.0";
  return "v" + std::string(canonical);
}

Error validation(std::string message, std::string pointer, const char* detail_code = nullptr,
                 std::string detail_message = {}) {
  Error e = make_error(ErrorKind::Validation, std::move(message));
  e.path = std::move(pointer);
  if (detail_code) {
    e.details = Json::array({Json::object({{"path", Json(e.path)},
                                           {"code", Json(detail_code)},
                                           {"message", Json(detail_message.empty() ? e.message : detail_message)}})});
  }
  return e;
}

Error catalog_error(std::string message, const std::string& surface_id) {
  Error e = make_error(ErrorKind::Catalog, std::move(message));
  e.surface_id = surface_id;
  return e;
}

std::string component_pointer(size_t index) { return "/components/" + std::to_string(index); }

const char* code_for_kind(ErrorKind kind) {
  switch (kind) {
    case ErrorKind::Recursion: return "RECURSION_LIMIT_EXCEEDED";
    case ErrorKind::Integrity: return "INTEGRITY_ERROR";
    case ErrorKind::Catalog: return "CATALOG_NOT_FOUND";
    case ErrorKind::State: return "SURFACE_NOT_FOUND";
    case ErrorKind::Data: return "DATA_ERROR";
    case ErrorKind::Expression: return "EXPRESSION_ERROR";
    case ErrorKind::Validation: return "VALIDATION_FAILED";
    case ErrorKind::Parse: return "PARSING_FAILED";
    default: return "INTERNAL_ERROR";
  }
}

// Envelope context for errors raised before an operation exists (the
// Python mapper reads the last message of the payload).
struct EnvelopeContext {
  std::string version = "1.0";
  std::string action;
  std::string surface_id;
  std::string function_call_id;
};

EnvelopeContext envelope_context(const Json& payload, int depth = 0) {
  EnvelopeContext ctx;
  const Json* msg = &payload;
  if (payload.is_object() && payload["messages"].is_array() && !payload["messages"].empty() && depth == 0) {
    return envelope_context(payload["messages"][payload["messages"].size() - 1], depth + 1);
  }
  if (payload.is_array()) {
    if (payload.empty() || depth > 0) return ctx;
    return envelope_context(payload[payload.size() - 1], depth + 1);
  }
  if (!msg->is_object()) return ctx;
  static const char* const kActions[] = {"createSurface",        "updateComponents", "updateDataModel",
                                         "deleteSurface",        "callRendererFunction",
                                         "agentFunctionResponse", "beginRendering",   "surfaceUpdate",
                                         "dataModelUpdate"};
  for (const char* a : kActions) {
    if (msg->contains(a)) {
      ctx.action = a;
      break;
    }
  }
  if (const Json& v = (*msg)["version"]; v.is_string()) {
    const std::string c = normalize_protocol_version(v.as_string());
    if (!c.empty()) ctx.version = version_label(c);
  } else if (ctx.action == "beginRendering" || ctx.action == "surfaceUpdate" || ctx.action == "dataModelUpdate") {
    ctx.version = "v0.8";
  }
  if (!ctx.action.empty()) {
    const Json& inner = (*msg)[ctx.action];
    if (inner["surfaceId"].is_string()) ctx.surface_id = inner["surfaceId"].as_string();
    if (inner["functionCallId"].is_string()) ctx.function_call_id = inner["functionCallId"].as_string();
  }
  return ctx;
}

std::string str_of(const Json& j) { return j.is_string() ? j.as_string() : std::string(); }

// The detail code of a validator issue keyword (the Python SDK's mapping).
const char* issue_code(const std::string& k) {
  if (k == "required" || k == "minProperties") return "missing_field";
  if (k == "additionalProperties" || k == "unevaluatedProperties") return "extra_field";
  if (k == "type" || k == "format" || k == "pattern" || k == "enum") return "type_mismatch";
  if (k == "identifier") return "invalid_identifier";
  if (k == "function") return "unrecognized_function";
  if (k == "allowedCallers") return "invalid_function_call";
  return "invalid_value";
}

// Rebases validator issue paths (pointers into the item) under `base`.
void append_issue_details(const Error& e, const std::string& base, const std::string& surface_id, Json* details) {
  for (const auto& d : e.details.as_array()) {
    std::string p = d["path"].is_string() ? d["path"].as_string() : std::string();
    // A missing property is reported at the property, as the other SDKs do.
    if (str_of(d["keyword"]) == "required") {
      const std::string m = str_of(d["message"]);
      const size_t q = m.find('\'');
      const size_t q2 = q == std::string::npos ? q : m.find('\'', q + 1);
      if (q2 != std::string::npos) p += "/" + escape_pointer_token(m.substr(q + 1, q2 - q - 1));
    }
    Json out = Json::object({{"path", Json(base + p)},
                             {"code", d["code"].is_string() ? d["code"] : Json(issue_code(str_of(d["keyword"])))},
                             {"message", d["message"]}});
    if (!surface_id.empty()) out.set("surfaceId", Json(surface_id));
    details->push_back(std::move(out));
  }
}

}  // namespace

Json map_error_to_renderer_message(const Error& error, std::string_view version) {
  const std::string code = error.code.empty() ? std::string(code_for_kind(error.kind)) : error.code;
  Json body = Json::object({{"code", Json(code)}});
  if (code == "INVALID_FUNCTION_CALL") {
    body.set("functionCallId", Json(error.function_call_id));
  } else {
    body.set("surfaceId", Json(error.surface_id));
    std::string path = error.path;
    if (path.empty() && error.kind != ErrorKind::Catalog) {
      const Json& first = error.details[size_t{0}];
      if (first["path"].is_string()) path = first["path"].as_string();
    }
    if (!path.empty()) body.set("path", Json(path));
  }
  body.set("message", Json(error.message));
  return Json::object({{"version", Json(std::string(version.empty() ? "1.0" : version))}, {"error", std::move(body)}});
}

// ---------------------------------------------------------------------------

struct MessageProcessor::CallState {
  const ValidationConfig* config;  // null: no validation
  bool user_activated;
};

MessageProcessor::MessageProcessor(std::vector<CatalogPtr> catalogs, MessageProcessorOptions options)
    : validation_config_(options.validation_config),
      outbound_(std::move(options.outbound_listener)),
      adapters_(options.adapters ? std::move(options.adapters) : VersionAdapterRegistry::make_default()),
      rpc_(options.rpc) {
  for (auto& c : catalogs) {
    if (!c) continue;
    bool dup = false;
    for (const auto& k : catalogs_) dup = dup || k->id() == c->id();
    if (!dup) catalogs_.push_back(std::move(c));
  }
}

MessageProcessor::~MessageProcessor() { dispose(); }

CatalogPtr MessageProcessor::find_catalog(std::string_view id) const {
  for (const auto& c : catalogs_) {
    if (c->id() == id) return c;
  }
  return nullptr;
}

void MessageProcessor::send_outbound(const Json& message) const {
  if (outbound_) outbound_(message);
}

Status MessageProcessor::process_text(std::string_view text, const ExecutionContext& context) {
  auto parsed = Json::parse(text, 128);
  if (!parsed.ok) {
    Error e = make_error(ErrorKind::Parse, "Invalid JSON payload: " + parsed.error + " at offset " +
                                               std::to_string(parsed.offset));
    return report(std::move(e), Json(), nullptr);
  }
  return process_messages(parsed.value, context);
}

Status MessageProcessor::process_messages(const Json& payload, const ExecutionContext& context) {
  if (disposed_) return make_error(ErrorKind::State, "The message processor has been disposed.");
  const ValidationConfig* config = nullptr;
  if (!context.skip_validation) {
    if (context.validation_config) {
      config = &*context.validation_config;
    } else if (validation_config_) {
      config = &*validation_config_;
    }
  }
  CallState call{config, context.is_user_activated};

  auto adapter = adapters_->resolve_from_payload(payload);
  if (!adapter.ok()) return report(adapter.take_error(), payload, nullptr);
  std::vector<InternalOperation> ops;
  AdapterOptions adapter_options;
  adapter_options.allow_unknown_elements = config && config->allow_unknown_elements;
  Status s = adapter.value()->extract_operations(payload, adapter_options, &ops);
  if (!s.ok()) return report(s.take_error(), payload, nullptr);
  for (const auto& op : ops) {
    Status r = run_operation(op, call);
    if (!r.ok()) return report(r.take_error(), payload, &op);
  }
  return ok_status();
}

Error MessageProcessor::report(Error error, const Json& payload, const InternalOperation* op) {
  EnvelopeContext ctx = envelope_context(payload);
  if (op) {
    ctx.version = version_label(op->version);
    ctx.action = InternalOperation::type_name(op->type);
    ctx.surface_id = op->surface_id;
    ctx.function_call_id = op->function_call_id;
  }
  if (error.surface_id.empty()) error.surface_id = ctx.surface_id;
  if (error.function_call_id.empty()) error.function_call_id = ctx.function_call_id;
  if (error.code.empty() && ctx.action == "callRendererFunction") error.code = "INVALID_FUNCTION_CALL";
  Json envelope = map_error_to_renderer_message(error, ctx.version);
  const Json& body = envelope["error"];
  if (error.code.empty()) error.code = body["code"].as_string();
  if (error.path.empty() && body["path"].is_string()) error.path = body["path"].as_string();
  if (!error.surface_id.empty()) {
    if (SurfaceModel* s = model_.get_surface(error.surface_id)) s->dispatch_error(body);
  }
  on_error_.emit(envelope);
  send_outbound(envelope);
  return error;
}

Status MessageProcessor::run_operation(const InternalOperation& op, CallState& call) {
  using T = InternalOperation::Type;
  switch (op.type) {
    case T::CreateSurface: return create_surface(op, call);
    case T::UpdateComponents: return update_components(op.surface_id, op.components, call);
    case T::UpdateDataModel: return update_data_model(op);
    case T::DeleteSurface:
      if (model_.get_surface(op.surface_id)) {
        model_.delete_surface(op.surface_id);
        if (rpc_) rpc_->cancel_surface_calls(op.surface_id);
      }
      return ok_status();
    case T::CallRendererFunction:
      if (rpc_) {
        SurfaceModel* surface = op.surface_id.empty() ? nullptr : model_.get_surface(op.surface_id);
        InternalOperation forwarded = op;
        forwarded.user_activated = op.user_activated || call.user_activated;
        rpc_->handle_call_renderer_function(forwarded, surface, *this);
      }
      return ok_status();
    case T::AgentFunctionResponse:
      if (rpc_) rpc_->handle_agent_function_response(op, *this);
      return ok_status();
  }
  return ok_status();
}

Status MessageProcessor::create_surface(const InternalOperation& op, CallState& call) {
  const std::string& sid = op.surface_id;
  const bool v1 = op.version == "1.0";
  CatalogPtr surface_catalog;
  if (op.catalog_id) {
    surface_catalog = find_catalog(*op.catalog_id);
    if (!surface_catalog) return catalog_error("Catalog not found: " + *op.catalog_id, sid);
  } else if (!v1) {
    if (catalogs_.empty()) return catalog_error("No default catalog available for surface.", sid);
    surface_catalog = catalogs_.front();
  }
  if (model_.get_surface(sid)) {
    Error e = make_error(ErrorKind::Integrity, "Surface " + sid + " already exists.");
    e.surface_id = sid;
    return e;
  }
  const std::string surface_version = surface_catalog ? surface_catalog->protocol_version() : op.version;
  if (surface_catalog && !is_catalog_version_compatible(surface_catalog->protocol_version(), op.version)) {
    return catalog_error("Surface '" + sid + "' catalog '" + surface_catalog->id() + "' specification version (" +
                             version_label(surface_catalog->protocol_version()) +
                             ") does not match message protocol version (" + version_label(op.version) + ").",
                         sid);
  }

  CatalogMap available;
  for (const auto& c : catalogs_) {
    if (is_catalog_version_compatible(c->protocol_version(), surface_version)) available.emplace(c->id(), c);
  }
  SurfaceOptions options;
  options.protocol_version = surface_version;
  options.send_data_model = op.send_data_model;
  options.metadata = op.metadata;
  if (op.root) options.root_id = *op.root;
  auto owned = std::make_unique<SurfaceModel>(sid, surface_catalog, std::move(available), std::move(options));
  SurfaceModel* surface = owned.get();
  A2UI_RETURN_IF_ERROR(model_.add_surface(std::move(owned)));

  // The data model first, one write per top-level key (blueprint
  // "createSurface operation ordering"), then the components.
  Status s = ok_status();
  for (const auto& m : op.data_model.as_object()) {
    if (m.first.empty()) {
      Json root = surface->data_model().root().is_object() ? surface->data_model().root() : Json::object();
      if (m.second.is_nullish()) {
        root.erase("");
      } else {
        root.set("", m.second);
      }
      s = surface->data_model().set("/", std::move(root));
    } else {
      s = surface->data_model().set("/" + escape_pointer_token(m.first), m.second);
    }
    if (!s.ok()) {
      Error e = s.take_error();
      if (e.path.empty()) e.path = "/" + escape_pointer_token(m.first);
      e.surface_id = sid;
      s = std::move(e);
      break;
    }
  }
  if (s.ok() && op.components.is_array()) s = update_components(sid, op.components, call);
  if (!s.ok()) {
    model_.delete_surface(sid);
    return s;
  }
  return ok_status();
}

Status MessageProcessor::build_components(SurfaceModel& surface, const Json& components, CallState& call,
                                          SurfaceComponentsModel::Batch* batch) {
  const std::string& sid = surface.id();
  const bool v1 = surface.is_v1();
  const CatalogPtr& default_catalog = surface.default_catalog_ptr();
  for (size_t idx = 0; idx < components.size(); ++idx) {
    const Json& comp = components[idx];
    const std::string at = component_pointer(idx);
    const Json& id_v = comp["id"];
    if (!id_v.is_string() || id_v.as_string().empty()) {
      Error e = validation("Component update payload is missing an 'id' / missing required 'id' field.", at + "/id");
      e.surface_id = sid;
      return e;
    }
    const std::string& id = id_v.as_string();
    const Json& type_v = comp["component"];
    if (!type_v.is_string() || type_v.as_string().empty()) {
      const ComponentModel* existing = surface.components_model().get(id);
      if (v1 || !existing) {
        Error e = validation("Component " + id + " is missing the required 'component' field.", at + "/component",
                             "missing_field", "'component' is a required property");
        e.surface_id = sid;
        return e;
      }
    }
    const std::string type = type_v.is_string() && !type_v.as_string().empty()
                                 ? type_v.as_string()
                                 : surface.components_model().get(id)->type();

    CatalogPtr comp_catalog;
    std::vector<std::string> custom;  // Bare envelope names the type declares as properties.
    auto custom_props = [&](const Catalog* cat) {
      const ComponentApi* api = cat ? cat->find_component(type) : nullptr;
      return api ? api->custom_envelope_properties : std::vector<std::string>();
    };
    auto is_custom = [&](const std::string& k) { return std::find(custom.begin(), custom.end(), k) != custom.end(); };
    Json properties = Json::object();
    Json metadata;

    if (!v1) {
      comp_catalog = default_catalog;
      if (!comp_catalog) {
        return catalog_error("Component " + id + " names no catalogId and surface " + sid +
                                 " has no default catalogId.",
                             sid);
      }
      for (const auto& m : comp.as_object()) {
        if (m.first != "id" && m.first != "component") properties.set(m.first, m.second);
      }
    } else {
      const bool has_at = comp.contains("@catalogId");
      const bool has_bare = comp.contains("catalogId");
      const std::vector<std::string> default_custom = custom_props(default_catalog.get());
      const bool bare_is_custom =
          has_bare && std::find(default_custom.begin(), default_custom.end(), "catalogId") != default_custom.end();
      const char* key = has_at ? "@catalogId" : (has_bare && !bare_is_custom ? "catalogId" : nullptr);
      if (key) {
        const Json& cid = comp[key];
        if (!cid.is_string()) {
          Error e = validation("Component " + id + " has a non-string '" + key + "'.", at + "/" + key, "type_mismatch",
                               std::string("'") + key + "' must be a string");
          e.surface_id = sid;
          return e;
        }
        comp_catalog = find_catalog(cid.as_string());
        if (has_at && has_bare) {
          const Catalog* check = comp_catalog ? comp_catalog.get() : default_catalog.get();
          auto cc = custom_props(check);
          if (std::find(cc.begin(), cc.end(), "catalogId") == cc.end()) {
            Error e = validation("Component '" + id + "' cannot specify both '@catalogId' and legacy 'catalogId'.",
                                 at + "/@catalogId", "conflicting_properties");
            e.surface_id = sid;
            return e;
          }
        }
        if (!comp_catalog) return catalog_error("Catalog not found: " + cid.as_string(), sid);
        if (!is_catalog_version_compatible(comp_catalog->protocol_version(), surface.protocol_version())) {
          return catalog_error("Component " + id + " catalog '" + cid.as_string() +
                                   "' has different protocol version " +
                                   version_label(comp_catalog->protocol_version()) + " than surface " + sid + " (" +
                                   version_label(surface.protocol_version()) + ").",
                               sid);
        }
      } else if (default_catalog) {
        comp_catalog = default_catalog;
      } else {
        return catalog_error("Component " + id + " names no catalogId and surface " + sid +
                                 " has no default catalogId.",
                             sid);
      }

      custom = custom_props(comp_catalog.get());
      if (has_bare && !has_at && !bare_is_custom) {
        custom.erase(std::remove(custom.begin(), custom.end(), std::string("catalogId")), custom.end());
      }
      static const char* const kPairs[][2] = {
          {"metadata", "@metadata"}, {"catalogId", "@catalogId"}, {"accessibility", "@accessibility"}, {"checks", "@checks"}};
      for (const auto& p : kPairs) {
        if (comp.contains(p[1]) && comp.contains(p[0]) && !is_custom(p[0])) {
          Error e = validation("Component '" + id + "' cannot specify both '" + p[1] + "' and legacy '" + p[0] + "'.",
                               at + "/" + p[1], "conflicting_properties");
          e.surface_id = sid;
          return e;
        }
      }
      const char* meta_key = comp.contains("@metadata")                         ? "@metadata"
                             : comp.contains("metadata") && !is_custom("metadata") ? "metadata"
                                                                                    : nullptr;
      if (meta_key) {
        const Json& raw = comp[meta_key];
        if (raw.is_null()) {
          Error e = validation("Component '" + id + "' " + meta_key + " cannot be null.", at + "/" + meta_key,
                               "type_mismatch", std::string("'") + meta_key + "' must be an object");
          e.surface_id = sid;
          return e;
        }
        Status ms = validate_metadata(raw, "component '" + id + "' metadata");
        if (!ms.ok()) {
          Error e = validation(ms.error().message, at + "/" + meta_key, "invalid_metadata");
          e.surface_id = sid;
          return e;
        }
        metadata = raw;
      }
      const ComponentApi* api = comp_catalog->find_component(type);
      const bool checkable = api && api->checkable;
      for (const auto& m : comp.as_object()) {
        const std::string& k = m.first;
        if (k == "id" || k == "component" || k == "@catalogId" || k == "@metadata") continue;
        if ((k == "catalogId" || k == "metadata") && !is_custom(k)) continue;
        if (k == "accessibility" && !is_custom(k)) {
          properties.set("@accessibility", m.second);
          continue;
        }
        if (k == "checks" && !is_custom(k) && checkable) {
          properties.set("@checks", m.second);
          continue;
        }
        properties.set(k, m.second);
      }
    }
    batch->push_back(
        std::make_unique<ComponentModel>(id, type, std::move(comp_catalog), std::move(properties), std::move(metadata)));
  }
  (void)call;
  return ok_status();
}

Status MessageProcessor::validate_nested_calls(SurfaceModel& surface, const SurfaceComponentsModel::Batch& batch,
                                               CallState& call, Json* details, std::string* summary,
                                               std::string* first_path) {
  const std::string& sid = surface.id();
  struct Frame {
    const Json* value;
    std::string path;
  };
  for (size_t idx = 0; idx < batch.size(); ++idx) {
    const ComponentModel& comp = *batch[idx];
    const std::string base = component_pointer(idx);
    std::vector<Frame> stack;
    for (auto it = comp.properties().as_object().rbegin(); it != comp.properties().as_object().rend(); ++it) {
      stack.push_back({&it->second, "/" + escape_pointer_token(it->first)});
    }
    while (!stack.empty()) {
      Frame f = std::move(stack.back());
      stack.pop_back();
      const Json& v = *f.value;
      if (v.is_array()) {
        for (size_t i = v.size(); i-- > 0;) stack.push_back({&v[i], f.path + "/" + std::to_string(i)});
        continue;
      }
      if (!v.is_object()) continue;
      for (auto it = v.as_object().rbegin(); it != v.as_object().rend(); ++it) {
        stack.push_back({&it->second, f.path + "/" + escape_pointer_token(it->first)});
      }
      const Json& name_v = v["@call"];
      if (!name_v.is_string() || (!name_v.as_string().empty() && name_v.as_string()[0] == '@')) continue;
      const std::string& name = name_v.as_string();
      const Json* cid = v.find("catalogId");
      if (cid && !cid->is_string()) {
        if (call.config) continue;  // Component validation reports it.
        const std::string msg = "Function call '" + name + "' in component '" + comp.id() + "' has a non-string 'catalogId'.";
        Json d = Json::object({{"path", Json(base + f.path + "/catalogId")},
                               {"code", Json("type_mismatch")},
                               {"message", Json("'catalogId' must be a string")}});
        details->push_back(std::move(d));
        if (!summary->empty()) *summary += "\n";
        *summary += msg;
        if (first_path->empty()) *first_path = base + f.path + "/catalogId";
        continue;
      }
      const std::string* cat_id = cid ? &cid->as_string() : nullptr;
      auto resolved = surface.resolve_catalog(cat_id, "Function call '" + name + "' in component '" + comp.id() + "'");
      if (!resolved.ok()) {
        if (cat_id && find_catalog(*cat_id)) {
          return catalog_error("Catalog '" + *cat_id + "' is not available on surface " + sid +
                                   ": its protocol version does not match the surface (" +
                                   version_label(surface.protocol_version()) + ").",
                               sid);
        }
        Error e = resolved.take_error();
        e.surface_id = sid;
        return e;
      }
      if (!call.config) continue;
      const CatalogPtr& catalog = resolved.value();
      const bool runs_in_component_catalog =
          cat_id ? *cat_id == comp.catalog().id() : comp.catalog_ptr() == surface.default_catalog_ptr();
      if (runs_in_component_catalog) continue;  // Component validation checks it.
      ComponentValidationOptions vo;
      vo.allow_unknown_functions = call.config->allow_unknown_elements;
      vo.allow_unknown_properties = call.config->allow_unknown_elements;
      vo.catalog_is_default = catalog == surface.default_catalog_ptr();
      Status s = validate_function_call(*catalog, v, nullptr, vo);
      if (s.ok() || s.error().kind == ErrorKind::Generic) continue;
      if (!summary->empty()) *summary += "\n";
      *summary += "Validation failed for function call '" + name + "' in component '" + comp.id() + "': " + s.error().message;
      const size_t before = details->size();
      append_issue_details(s.error(), base + f.path, sid, details);
      if (first_path->empty() && details->size() > before) *first_path = (*details)[before]["path"].as_string();
    }
  }
  return ok_status();
}

Status MessageProcessor::update_components(const std::string& sid, const Json& components, CallState& call) {
  SurfaceModel* surface = model_.get_surface(sid);
  if (!surface) {
    Error e = make_error(ErrorKind::Integrity, "Surface not found for message: " + sid);
    e.surface_id = sid;
    return e;
  }
  if (!components.is_array()) {
    Error e = validation("Components payload must be a list.", "/components");
    e.surface_id = sid;
    return e;
  }
  SurfaceComponentsModel::Batch batch;
  batch.reserve(components.size());
  A2UI_RETURN_IF_ERROR(build_components(*surface, components, call, &batch));

  // Nested calls: catalog resolution errors at once; argument errors are
  // reported together with the component schema errors.
  Json nested_details{Json::Array{}};
  std::string nested_summary, nested_first;
  if (surface->is_v1()) {
    A2UI_RETURN_IF_ERROR(validate_nested_calls(*surface, batch, call, &nested_details, &nested_summary, &nested_first));
  }
  auto nested_error = [&]() {
    Error e = make_error(ErrorKind::Validation, nested_summary);
    e.details = nested_details;
    e.path = nested_first;
    e.surface_id = sid;
    return e;
  };

  // Duplicate ids within the batch.
  {
    std::set<std::string_view> seen;
    for (size_t i = 0; i < batch.size(); ++i) {
      const std::string& id = batch[i]->id();
      if (seen.insert(id).second) continue;
      const std::string msg = "Duplicate component ID '" + id + "' in update payload.";
      Error e = make_error(ErrorKind::Integrity, msg);
      e.path = component_pointer(i) + "/id";
      e.surface_id = sid;
      e.details = Json::array({Json::object({{"path", Json("/components/" + escape_pointer_token(id))},
                                             {"code", Json("duplicate_id")},
                                             {"message", Json(msg)}})});
      return e;
    }
  }

  // Component schemas.
  if (call.config) {
    const ValidationConfig& cfg = *call.config;
    Json details{Json::Array{}};
    std::string summary, first_path;
    for (size_t idx = 0; idx < batch.size(); ++idx) {
      const ComponentModel& c = *batch[idx];
      const std::string base = component_pointer(idx);
      auto add = [&](const std::string& rel, const char* code, const std::string& msg) {
        Json d = Json::object({{"path", Json(base + rel)}, {"code", Json(code)}, {"message", Json(msg)}});
        d.set("surfaceId", Json(sid));
        details.push_back(std::move(d));
        if (first_path.empty()) first_path = base + rel;
        if (!summary.empty()) summary += "\n";
        summary += "Component '" + c.id() + "' (" + c.type() + "): " + msg;
      };
      if (!c.catalog().is_v1()) continue;  // Pre-1.0 schemas are out of scope.
      if (!is_uax31_identifier(c.id())) {
        add("/id", "invalid_identifier", "Component id '" + c.id() + "' must be a valid UAX #31 identifier");
      }
      if (!is_uax31_identifier(c.type())) {
        add("/component", "invalid_identifier",
            "Component type '" + c.type() + "' must be a valid UAX #31 identifier");
      }
      ComponentApi stub;
      const ComponentApi* api = c.api();
      if (!api) {
        if (!cfg.allow_unknown_elements) {
          add("/component", "unrecognized_component", "Unrecognized component type '" + c.type() + "'");
        }
        stub.name = c.type();
        api = &stub;
      }
      Json tree = c.component_tree();
      const bool in_default = c.catalog_ptr() == surface->default_catalog_ptr();
      if (!in_default) tree.set("@catalogId", Json(c.catalog().id()));
      ComponentValidationOptions vo;
      vo.allow_unknown_properties = cfg.allow_unknown_elements || api == &stub;
      vo.allow_unknown_functions = cfg.allow_unknown_elements;
      vo.catalog_is_default = in_default;
      Status s = validate_component_properties(c.catalog(), *api, tree, nullptr, vo);
      if (!s.ok() && s.error().kind != ErrorKind::Generic) {
        const size_t before = details.size();
        append_issue_details(s.error(), base, sid, &details);
        if (first_path.empty() && details.size() > before) first_path = details[before]["path"].as_string();
        if (!summary.empty()) summary += "\n";
        summary += s.error().message;
      }
    }
    if (!details.empty()) {
      Error e = make_error(ErrorKind::Validation, summary, "VALIDATION_FAILED");
      e.surface_id = sid;
      e.path = first_path;
      if (!nested_details.empty()) {
        // Merge, keeping each detail once.
        Json merged = nested_details;
        for (const auto& d : details.as_array()) {
          bool dup = false;
          for (const auto& n : nested_details.as_array()) {
            dup = dup || (n["path"] == d["path"] && n["message"] == d["message"]);
          }
          if (!dup) merged.push_back(d);
        }
        e.message = nested_summary + "\n" + e.message;
        e.path = nested_first.empty() ? first_path : nested_first;
        e.details = std::move(merged);
      } else {
        e.details = std::move(details);
      }
      return e;
    }
  }

  // Composition constraints and topology over the candidate graph.
  GraphCheckOptions graph;
  graph.root_id = surface->root_id();
  graph.surface_id = sid;
  std::optional<ValidationConfig> cfg;
  if (call.config) cfg = *call.config;
  Status g = surface->components_model().validate_components_update(batch, cfg, graph);
  if (!nested_details.empty()) return nested_error();
  if (!g.ok()) return g;

  // Apply.
  {
    Batch signals;
    SurfaceComponentsModel& model = surface->components_model();
    for (auto& c : batch) {
      ComponentModel* existing = model.get(c->id());
      if (existing && (existing->type() != c->type() || existing->catalog_ptr() != c->catalog_ptr())) {
        model.replace_component(std::move(c));
      } else if (existing) {
        existing->apply_update(c->properties(), c->metadata());
      } else {
        (void)model.add_component(std::move(c));
      }
    }
    model.batch_applied();
  }
  return ok_status();
}

Status MessageProcessor::update_data_model(const InternalOperation& op) {
  SurfaceModel* surface = model_.get_surface(op.surface_id);
  if (!surface) {
    Error e = make_error(ErrorKind::Integrity, "Surface not found for message: " + op.surface_id);
    e.surface_id = op.surface_id;
    return e;
  }
  const std::string& p = op.path;
  bool valid = p.empty() || p[0] == '/';
  for (size_t i = 0; valid && i < p.size(); ++i) {
    if (p[i] == '~' && (i + 1 >= p.size() || (p[i + 1] != '0' && p[i + 1] != '1'))) valid = false;
  }
  if (!valid) {
    const std::string msg = "Invalid path syntax: '" + p +
                            "' (path must be a valid RFC 6901 JSON Pointer starting with '/')";
    Error e = validation(msg, "/path", "invalid_value");
    e.surface_id = op.surface_id;
    return e;
  }
  Status s = surface->data_model().set(p.empty() ? std::string("/") : p, op.value);
  if (!s.ok()) {
    Error e = s.take_error();
    if (e.path.empty()) e.path = p;
    e.surface_id = op.surface_id;
    return e;
  }
  return ok_status();
}

Result<Json> MessageProcessor::get_renderer_capabilities(const CapabilitiesOptions& options) const {
  if (options.versions.empty()) {
    return make_error(ErrorKind::Validation,
                      "At least one protocol version must be provided in CapabilitiesOptions to generate renderer "
                      "capabilities.");
  }
  Json result = Json::object();
  for (const auto& version : options.versions) {
    const std::string canonical = normalize_protocol_version(version);
    std::vector<CatalogPtr> compatible;
    for (const auto& c : catalogs_) {
      if (is_catalog_version_compatible(c->protocol_version(), canonical)) compatible.push_back(c);
    }
    std::sort(compatible.begin(), compatible.end(),
              [](const CatalogPtr& a, const CatalogPtr& b) { return a->id() < b->id(); });
    Json ids{Json::Array{}};
    for (const auto& c : compatible) ids.push_back(Json(c->id()));
    Json caps = Json::object({{"supportedCatalogIds", std::move(ids)}});
    if (options.include_inline_catalogs) {
      if (canonical != "1.0") {
        return make_error(ErrorKind::Validation,
                          "Inline catalogs are generated for protocol 1.0 only, not '" + version + "'.");
      }
      Json inline_catalogs{Json::Array{}};
      for (const auto& c : compatible) {
        Json doc = c->to_json();
        doc.set("catalogId", Json(c->id()));
        doc.set("protocolVersion", Json("1.0"));
        inline_catalogs.push_back(std::move(doc));
      }
      caps.set("inlineCatalogs", std::move(inline_catalogs));
    }
    result.set(version, std::move(caps));
  }
  return result;
}

Result<Json> MessageProcessor::get_renderer_data_model(std::string_view version) const {
  std::vector<const SurfaceModel*> enabled;
  for (const auto& s : model_.surfaces()) {
    if (s.second->send_data_model()) enabled.push_back(s.second.get());
  }
  if (enabled.empty()) return Json(nullptr);
  Json surfaces = Json::object();
  std::string label;
  if (!version.empty()) {
    label = std::string(version);
    for (const SurfaceModel* s : enabled) {
      if (s->protocol_version().empty() || is_catalog_version_compatible(s->protocol_version(), version)) {
        surfaces.set(s->id(), s->data_model().root());
      }
    }
    if (surfaces.empty()) return Json(nullptr);
  } else {
    std::set<std::string> versions;
    for (const SurfaceModel* s : enabled) {
      if (!s->protocol_version().empty()) versions.insert(version_label(s->protocol_version()));
    }
    if (versions.size() > 1) {
      std::string list;
      for (const auto& v : versions) list += (list.empty() ? "" : ", ") + std::string("'") + v + "'";
      return make_error(ErrorKind::Validation, "Multiple protocol versions detected among active surfaces: [" + list +
                                                   "]. Specify a target protocol version in "
                                                   "get_renderer_data_model(version).");
    }
    label = versions.empty() ? std::string("1.0") : *versions.begin();
    for (const SurfaceModel* s : enabled) surfaces.set(s->id(), s->data_model().root());
  }
  return Json::object({{"version", Json(label)}, {"surfaces", std::move(surfaces)}});
}

void MessageProcessor::dispose() {
  if (disposed_) return;
  disposed_ = true;
  model_.dispose();
  if (rpc_) rpc_->dispose();
  on_error_.clear();
}

}  // namespace a2ui

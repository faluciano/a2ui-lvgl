// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// SurfaceModel and SurfaceGroupModel. See a2ui/surface_model.h.

#include "a2ui/surface_model.h"

#include <cmath>
#include <cstdio>
#include <ctime>
#include <utility>

#include "a2ui/metadata.h"
#include "a2ui/schema.h"

#if defined(__unix__) || defined(__APPLE__) || defined(ESP_PLATFORM)
#include <sys/time.h>
#endif

namespace a2ui {
namespace {

double system_seconds() {
#if defined(__unix__) || defined(__APPLE__) || defined(ESP_PLATFORM)
  timeval tv{};
  gettimeofday(&tv, nullptr);
  return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) / 1e6;
#else
  return static_cast<double>(std::time(nullptr));
#endif
}

const char* code_for_function_error(const Error& e) {
  switch (e.kind) {
    case ErrorKind::Catalog: return "CATALOG_ERROR";
    case ErrorKind::Expression: return "EXPRESSION_ERROR";
    case ErrorKind::Validation:
    case ErrorKind::Integrity:
    case ErrorKind::Recursion: return "VALIDATION_FAILED";
    case ErrorKind::Data: return "DATA_ERROR";
    default: return "EXECUTION_ERROR";
  }
}

Error invalid_action(const std::string& message) {
  return make_error(ErrorKind::Validation, message, "INVALID_ACTION");
}

}  // namespace

std::string iso8601_utc(double seconds) {
  const double whole = std::floor(seconds);
  const std::time_t t = static_cast<std::time_t>(whole);
  int ms = static_cast<int>(std::lround((seconds - whole) * 1000.0));
  if (ms > 999) ms = 999;
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  // Sized for the worst case GCC assumes (every int field at 11 chars), so
  // -Wformat-truncation stays quiet on the ESP-IDF toolchains.
  char buf[96];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
  return buf;
}

bool is_catalog_version_compatible(std::string_view catalog_version, std::string_view message_version) {
  const std::string a = normalize_protocol_version(catalog_version);
  const std::string b = normalize_protocol_version(message_version);
  return !a.empty() && a == b;
}

// ---------------------------------------------------------------------------
// SurfaceModel

SurfaceModel::SurfaceModel(std::string id, CatalogPtr default_catalog, CatalogMap available_catalogs,
                           SurfaceOptions options)
    : id_(std::move(id)),
      default_catalog_(std::move(default_catalog)),
      available_catalogs_(std::move(available_catalogs)),
      protocol_version_(normalize_protocol_version(options.protocol_version)),
      send_data_model_(options.send_data_model),
      root_id_(options.root_id.empty() ? std::string("root") : std::move(options.root_id)),
      data_model_(options.data_model.is_object() ? std::move(options.data_model) : Json::object()),
      metadata_(normalize_metadata(options.metadata)) {
  if (protocol_version_.empty() && default_catalog_) protocol_version_ = default_catalog_->protocol_version();
  if (default_catalog_ && !default_catalog_->id().empty() &&
      available_catalogs_.find(default_catalog_->id()) == available_catalogs_.end()) {
    available_catalogs_.emplace(default_catalog_->id(), default_catalog_);
  }
}

SurfaceModel::~SurfaceModel() { dispose(); }

Result<CatalogPtr> SurfaceModel::resolve_catalog(const std::string* catalog_id, std::string_view subject) const {
  if (catalog_id) {
    auto it = available_catalogs_.find(*catalog_id);
    if (it == available_catalogs_.end()) {
      return make_error(ErrorKind::Catalog, "Catalog not found: " + *catalog_id);
    }
    return it->second;
  }
  if (!default_catalog_) {
    return make_error(ErrorKind::Catalog, std::string(subject) + " names no catalogId and surface " + id_ +
                                              " has no default catalogId.");
  }
  return default_catalog_;
}

Status SurfaceModel::set_metadata(const Json& metadata) {
  Json next;
  auto changed = metadata_prepare(metadata_.value(), metadata, "surface '" + id_ + "' metadata", &next);
  if (!changed.ok()) return changed.take_error();
  if (!changed.value()) return ok_status();
  metadata_.set_force(std::move(next));
  on_metadata_changed_.emit(metadata_.value());
  return ok_status();
}

Status SurfaceModel::set_extension(const std::string& key, Json value) {
  auto next = metadata_with_extension(metadata_.value(), key, std::move(value), "surface '" + id_ + "' metadata");
  if (!next.ok()) return next.take_error();
  return set_metadata(next.value());
}

void SurfaceModel::remove_extension(const std::string& key) {
  (void)set_metadata(metadata_without_extension(metadata_.value(), key));
}

DataContext SurfaceModel::data_context(const std::string& path, bool user_activated) {
  return DataContext(data_model_, this, path, std::nullopt, user_activated);
}

Json SurfaceModel::resolve_action(const Json& action, const std::string& scope_path, bool user_activated) {
  if (!action.is_object()) return action;
  // Inside a template the scope's last segment is the item index (@index).
  std::optional<long> index;
  const auto segments = path::split(scope_path);
  if (!segments.empty()) {
    const long i = path::parse_index(segments.back());
    if (i >= 0) index = i;
  }
  DataContext ctx(data_model_, this, scope_path, index, user_activated);

  // Resolves context values and userMessage of an event object in place.
  auto resolve_event = [&](Json evt) -> Json {
    if (Json* context = evt.find_mut("context"); context && context->is_object()) {
      Json resolved = Json::object();
      for (const auto& m : context->as_object()) resolved.set(m.first, ctx.resolve_value(m.second));
      *context = std::move(resolved);
    }
    if (Json* um = evt.find_mut("userMessage"); um && !um->is_nullish()) {
      Json v = ctx.resolve_value(*um);
      if (!ctx.had_failure() && !v.is_nullish() && !v.is_string()) {
        report_function_error(invalid_action("Action event 'userMessage' must resolve to a string."), "");
        return Json(nullptr);
      }
      *um = std::move(v);
    }
    if (ctx.had_failure()) return Json(nullptr);
    return evt;
  };

  if (const Json* evt = action.find("event"); evt && evt->is_object()) {
    const Json& name = (*evt)["name"];
    if (!name.is_string() || name.as_string().empty()) {
      report_function_error(invalid_action("Action event is missing required string 'name'."), "");
      return Json(nullptr);
    }
    Json resolved_evt = resolve_event(*evt);
    if (resolved_evt.is_null()) return resolved_evt;
    Json out = action;
    out.set("event", std::move(resolved_evt));
    return out;
  }
  if (action.contains("name")) return resolve_event(action);

  const Json* call = nullptr;
  if (const Json* fc = action.find("functionCall"); fc && fc->is_object()) {
    call = fc;
  } else if (action["@call"].is_string()) {
    call = &action;
  }
  if (call) {
    Json result = ctx.resolve_value(*call);
    return ctx.had_failure() ? Json(nullptr) : result;
  }
  return action;
}

Status SurfaceModel::dispatch_action(const Json& payload, const std::string& source_component_id,
                                     const Json& metadata) {
  if (!payload.is_object() || disposed_) return ok_status();
  const Json* evt = &payload;
  if (const Json* e = payload.find("event"); e && e->is_object()) {
    evt = e;
  } else if (!payload.contains("name")) {
    return ok_status();
  }
  const Json& name = (*evt)["name"];
  if (!name.is_string() || name.as_string().empty()) return ok_status();

  const Json* catalog_id = payload.find("catalogId");
  if (!catalog_id || !catalog_id->is_string() || catalog_id->as_string().empty()) catalog_id = evt->find("catalogId");

  const Json payload_metadata = evt->contains("metadata") ? (*evt)["metadata"] : payload["metadata"];
  Json effective;
  if (!payload_metadata.is_nullish() && !metadata.is_nullish()) {
    A2UI_RETURN_IF_ERROR(validate_metadata(payload_metadata, "action metadata"));
    A2UI_RETURN_IF_ERROR(validate_metadata(metadata, "action metadata"));
    Json ext = Json::object();
    for (const auto& m : payload_metadata["extensions"].as_object()) ext.set(m.first, m.second);
    for (const auto& m : metadata["extensions"].as_object()) ext.set(m.first, m.second);
    if (!ext.empty()) effective = Json::object({{"extensions", std::move(ext)}});
  } else if (!metadata.is_nullish()) {
    effective = metadata;
  } else {
    effective = payload_metadata;
  }
  if (!effective.is_nullish()) {
    A2UI_RETURN_IF_ERROR(validate_metadata(effective, "action metadata"));
    effective = normalize_metadata(effective);
  }

  Json action = Json::object({{"name", name},
                              {"surfaceId", Json(id_)},
                              {"sourceComponentId", Json(source_component_id)},
                              {"timestamp", Json(iso8601_utc(clock_ ? clock_() : system_seconds()))},
                              {"context", (*evt)["context"].is_object() ? (*evt)["context"] : Json::object()}});
  if (catalog_id && catalog_id->is_string() && !catalog_id->as_string().empty()) action.set("catalogId", *catalog_id);
  if (const Json& um = (*evt)["userMessage"]; um.is_string() && !um.as_string().empty()) {
    action.set("userMessage", um);
  }
  if (!effective.is_nullish()) action.set("metadata", effective);
  on_action_.emit(action);
  return ok_status();
}

void SurfaceModel::dispatch_error(Json error) {
  if (!error.is_object()) return;
  if (error["functionCallId"].is_string() && !error["functionCallId"].as_string().empty()) {
    error.erase("surfaceId");
  } else {
    error.set("surfaceId", Json(id_));
  }
  on_error_.emit(error);
}

Result<const Catalog*> SurfaceModel::resolve_function_catalog(const std::string* catalog_id,
                                                              const std::string& function_name) {
  auto r = resolve_catalog(catalog_id, "Function call '" + function_name + "'");
  if (!r.ok()) return r.take_error();
  return r.value().get();
}

void SurfaceModel::report_function_error(const Error& error, const std::string& function_name) {
  on_function_error_.emit(error);
  Json payload = Json::object(
      {{"code", Json(error.code.empty() ? std::string(code_for_function_error(error)) : error.code)},
       {"message", Json(error.message)}});
  if (!error.path.empty()) payload.set("path", Json(error.path));
  if (!error.function_call_id.empty()) payload.set("functionCallId", Json(error.function_call_id));
  dispatch_error(std::move(payload));
}

Status SurfaceModel::validate_function_args(const FunctionApi& api, const Json& args) {
  const Json& schema = api.args_schema;
  if (!schema.is_object() || !(schema.contains("type") || schema.contains("properties"))) return ok_status();
  // The catalog that declares this function provides the schema documents.
  for (const auto& entry : available_catalogs_) {
    const Catalog& cat = *entry.second;
    const FunctionDecl* decl = cat.find_function(api.name);
    if (!decl || &decl->api != &api) continue;
    if (!cat.is_v1()) return ok_status();
    SchemaRegistry reg = cat.schema_registry();
    return validate_schema(reg, "catalog.json", schema, args);
  }
  return ok_status();
}

bool SurfaceModel::agent_function_value(const std::string& name, const std::string& catalog_id, const Json& args,
                                        Json* value) {
  return agent_handler_ ? agent_handler_(name, catalog_id, args, value) : false;
}

Status SurfaceModel::open_url(const std::string& url) {
  if (url_handler_) return url_handler_(url);
  return ok_status();  // Headless: the request is accepted and dropped.
}

void SurfaceModel::dispose() {
  if (disposed_) return;
  disposed_ = true;
  agent_handler_ = nullptr;
  url_handler_ = nullptr;
  data_model_.dispose();
  components_.dispose();
  on_action_.clear();
  on_error_.clear();
  on_function_error_.clear();
  on_metadata_changed_.clear();
  metadata_.clear_listeners();
}

// ---------------------------------------------------------------------------
// SurfaceGroupModel

SurfaceGroupModel::~SurfaceGroupModel() { dispose(); }

Status SurfaceGroupModel::add_surface(std::unique_ptr<SurfaceModel> surface) {
  if (!surface) return make_error(ErrorKind::State, "Cannot add a null surface.");
  if (surfaces_.find(surface->id()) != surfaces_.end()) {
    Error e = make_error(ErrorKind::Integrity, "Surface " + surface->id() + " already exists.");
    e.surface_id = surface->id();
    return e;
  }
  SurfaceModel* raw = surface.get();
  const std::string id = raw->id();
  surfaces_.emplace(id, std::move(surface));
  const EventSource<Json>* group_actions = &on_action_;
  action_subs_[id] = raw->on_action().subscribe([group_actions](const Json& a) { group_actions->emit(a); });
  on_surface_created_.emit(raw);
  return ok_status();
}

void SurfaceGroupModel::delete_surface(const std::string& id) {
  auto it = surfaces_.find(id);
  if (it == surfaces_.end()) return;
  std::unique_ptr<SurfaceModel> s = std::move(it->second);
  surfaces_.erase(it);
  action_subs_.erase(id);
  s->dispose();
  on_surface_deleted_.emit(id);
}

SurfaceModel* SurfaceGroupModel::get_surface(std::string_view id) const {
  auto it = surfaces_.find(id);
  return it == surfaces_.end() ? nullptr : it->second.get();
}

void SurfaceGroupModel::dispose() {
  std::vector<std::string> ids;
  for (const auto& s : surfaces_) ids.push_back(s.first);
  for (const auto& id : ids) delete_surface(id);
  on_surface_created_.clear();
  on_surface_deleted_.clear();
  on_action_.clear();
}

}  // namespace a2ui

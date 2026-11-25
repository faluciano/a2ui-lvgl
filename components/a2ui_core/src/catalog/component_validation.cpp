// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Component and function call validation against one catalog. Ported from
// the Python core's PayloadValidator (validate_component,
// _validate_nested_functions, validate_function). See a2ui/validation.h.

#include "a2ui/validation.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "a2ui/identifier.h"
#include "a2ui/spec_documents.h"

namespace a2ui {
namespace {

constexpr int kMaxWalkDepth = 64;

Ref<const Json> parse_static(const char* text) {
  auto r = Json::parse(text);
  return make_ref<const Json>(std::move(r.value));
}

// The v1.0 FunctionCall checked as an envelope only: which function runs,
// and with which args, is checked per call against the catalog it runs in.
const Ref<const Json>& function_call_envelope() {
  static const Ref<const Json> s = parse_static(R"({
    "type": "object",
    "properties": {
      "@call": {"type": "string"},
      "catalogId": {"type": "string"},
      "args": {"type": "object"}
    },
    "required": ["@call"],
    "additionalProperties": false
  })");
  return s;
}

// The catalog.json stand-in for validating common types on their own.
const Ref<const Json>& stand_in_catalog() {
  static const Ref<const Json> s = parse_static(R"({
    "$defs": {
      "anyFunction": {
        "type": "object",
        "properties": {
          "@call": {"type": "string", "not": {"const": "@index"}},
          "args": {"type": "object"}
        },
        "required": ["@call"]
      },
      "anyComponent": {
        "type": "object",
        "properties": {"component": {"type": "string"}},
        "required": ["component"]
      }
    }
  })");
  return s;
}

bool is_single_at_key(std::string_view k) { return k.size() >= 1 && k[0] == '@' && (k.size() < 2 || k[1] != '@'); }

class Checker {
 public:
  Checker(const Catalog& catalog, const ComponentValidationOptions& opt)
      : catalog_(catalog), opt_(opt), reg_(catalog.schema_registry()) {
    reg_.add_override("common_types.json#/$defs/FunctionCall", function_call_envelope());
    // A catalog-local "#/$defs/FunctionCall" the catalog leaves undefined is the common one.
    if (!catalog.source_node()["$defs"].contains("FunctionCall")) {
      reg_.add_override("catalog.json#/$defs/FunctionCall", function_call_envelope());
    }
  }

  std::vector<ValidationIssue> issues;

  void issue(const std::string& path, const char* keyword, std::string message) {
    if (issues.size() < 32) issues.push_back({path, keyword, std::move(message)});
  }

  void schema_check(SchemaNode schema, const Json& instance, const std::string& path) {
    std::vector<ValidationIssue> found;
    SchemaValidateOptions o;
    o.max_issues = 8;
    Status s = validate_schema(reg_, "catalog.json", schema, instance, &found, o);
    if (s.ok()) return;
    for (auto& i : found) issue(path + i.instance_path, i.keyword.c_str(), std::move(i.message));
    if (found.empty()) issue(path, "schema", s.error().message);
  }

  // Walks a value for nested calls and reserved keys. `root` is the
  // component object itself.
  void walk(const Json& val, std::string& path, bool root, bool comp_in_default, int depth) {
    if (depth > kMaxWalkDepth) {
      issue(path, "depth", "value nesting exceeds the depth limit");
      return;
    }
    if (val.is_object()) {
      for (const auto& m : val.as_object()) {
        if (root && (m.first == "@metadata" || m.first == "@catalogId" ||
                     m.first == "@accessibility" || m.first == "@checks")) {
          continue;
        }
        if (is_single_at_key(m.first) && m.first != "@path" && m.first != "@call") {
          issue(path, "reservedKey",
                "Unrecognized reserved protocol directive '" + m.first +
                    "' in v1.0 dynamic object. Reserved keys must be @call or @path, or escaped "
                    "with prefix doubling (e.g. '@" + m.first + "').");
        }
      }
      if (const Json* name = val.find("@call"); name && name->is_string()) {
        nested_call(name->as_string(), val, path, comp_in_default);
      }
      for (const auto& m : val.as_object()) {
        if (root && (m.first == "id" || m.first == "component" || m.first == "@catalogId" ||
                     m.first == "@metadata")) {
          continue;
        }
        const size_t mark = path.size();
        path += '/';
        path += escape_pointer_token(m.first);
        walk(m.second, path, false, comp_in_default, depth + 1);
        path.resize(mark);
      }
    } else if (val.is_array()) {
      for (size_t i = 0; i < val.size(); ++i) {
        const size_t mark = path.size();
        path += '/';
        path += std::to_string(i);
        walk(val[i], path, false, comp_in_default, depth + 1);
        path.resize(mark);
      }
    }
  }

  void full_call_check(const std::string& name, const Json& call, const std::string& path) {
    const FunctionDecl* fn = catalog_.find_function(name);
    if (!fn) {
      if (opt_.allow_unknown_functions) {
        identifiers_check(name, call, path);
      } else {
        issue(path, "function", "Unrecognized function '" + name + "' in catalog '" + catalog_.id() + "'");
      }
      return;
    }
    if (fn->api.allowed_callers == AllowedCallers::AgentOnly) {
      issue(path, "allowedCallers",
            "Function '" + name + "' cannot be called by renderer (allowedCallers is agentOnly).");
    }
    const SchemaNode schema = fn->api.schema_node();
    if (schema["properties"].is_object()) {
      const Json wrapper = Json::object(
          {{"allOf", Json::array({Json::object({{"$ref", Json("common_types.json#/$defs/FunctionCommon")}}),
                                  Json::object({{"$ref", Json("catalog.json#/functions/" +
                                                              escape_pointer_token(name))}})})},
           {"unevaluatedProperties", Json(false)}});
      schema_check(wrapper, call, path);
      return;
    }
    // A definition without `properties` (minimal or shorthand form): check
    // the envelope and, when it is a schema, the args.
    schema_check(*function_call_envelope(), call, path);
    const SchemaNode args_schema = fn->api.args_schema_node();
    const Json& args = call["args"];
    if (args_schema.is_object() && (args_schema.contains("type") || args_schema.contains("properties"))) {
      schema_check(args_schema, args.is_undefined() ? Json::object() : args, path + "/args");
    }
  }

  void identifiers_check(const std::string& name, const Json& call, const std::string& path) {
    if (!is_uax31_identifier(name)) {
      issue(path + "/@call", "identifier", "Function name '" + name + "' must be a valid UAX #31 identifier");
    }
    const Json& args = call["args"];
    if (args.is_undefined()) return;
    if (!args.is_object()) {
      issue(path + "/args", "type", "Function arguments for '" + name + "' must be an object");
      return;
    }
    for (const auto& m : args.as_object()) {
      if (!is_uax31_identifier(m.first)) {
        issue(path + "/args/" + escape_pointer_token(m.first), "identifier",
              "Function argument '" + m.first + "' in function '" + name +
                  "' must be a valid UAX #31 identifier");
      }
    }
  }

  void nested_call(const std::string& name, const Json& call, const std::string& path,
                   bool comp_in_default) {
    if (!name.empty() && name[0] == '@') {
      if (call.contains("catalogId")) {
        issue(path + "/catalogId", "catalogId",
              "System function '" + name + "' belongs to no catalog and must not name a catalogId");
      }
      if (name == "@index") {
        const Json ref = Json::object({{"$ref", Json("common_types.json#/$defs/IndexSystemFunction")}});
        Json call_only = call;
        call_only.erase("catalogId");  // Reported above.
        schema_check(ref, call_only, path);
      } else {
        issue(path + "/@call", "function",
              "Unknown system function '" + name + "': v1.0 defines only @index");
      }
      return;
    }
    if (!is_uax31_identifier(name)) {
      issue(path + "/@call", "identifier", "Function name '" + name + "' must be a valid UAX #31 identifier");
      return;
    }
    const Json* cat = call.find("catalogId");
    bool full;
    if (cat && !cat->is_string()) {
      issue(path + "/catalogId", "type", "'catalogId' must be a string");
      full = false;
    } else if (cat) {
      full = cat->as_string() == catalog_.id();
    } else {
      full = comp_in_default;
    }
    if (full) {
      full_call_check(name, call, path);
    } else {
      identifiers_check(name, call, path);
    }
  }

  const Catalog& catalog_;
  const ComponentValidationOptions& opt_;
  SchemaRegistry reg_;
};

Status finish(std::vector<ValidationIssue>& found, std::vector<ValidationIssue>* out,
              const std::string& what) {
  if (found.empty()) return ok_status();
  const ValidationIssue& first = found.front();
  std::string msg = what + ": " + (first.instance_path.empty() ? std::string("/") : first.instance_path) +
                    ": " + first.message;
  if (found.size() > 1) msg += " (+" + std::to_string(found.size() - 1) + " more)";
  Error e = make_error(ErrorKind::Validation, std::move(msg), "VALIDATION_FAILED");
  e.details = issues_to_json(found);
  if (out) out->insert(out->end(), found.begin(), found.end());
  return e;
}

Status unsupported_version(const Catalog& catalog) {
  return make_error(ErrorKind::Generic, "Catalog '" + catalog.id() + "' targets protocol " +
                                            catalog.protocol_version() +
                                            "; payload validation supports v1.0 catalogs only");
}

}  // namespace

Status validate_component_properties(const Catalog& catalog, const ComponentApi& api,
                                     const Json& component, std::vector<ValidationIssue>* issues,
                                     const ComponentValidationOptions& options) {
  if (!catalog.is_v1()) return unsupported_version(catalog);
  Checker c(catalog, options);
  const std::string id = component["id"].is_string() ? component["id"].as_string() : std::string("?");
  const std::string what = "Component '" + id + "' (" + api.name + ") is invalid";
  if (!component.is_object()) {
    c.issue("", "type", "a component must be an object");
    return finish(c.issues, issues, what);
  }
  if (component["component"].is_string() && component["component"].as_string() != api.name) {
    c.issue("/component", "component",
            "component type '" + component["component"].as_string() + "' does not match '" + api.name + "'");
  }

  // Transition dual-read of bare envelope keys the schema does not declare.
  const Json* subject = &component;
  Json renamed;
  for (const char* bare : {"catalogId", "accessibility", "metadata", "checks"}) {
    if (!component.contains(bare) || api.declares_custom(bare)) continue;
    const std::string at = std::string("@") + bare;
    if (component.contains(at)) {
      c.issue("/" + std::string(bare), "envelope",
              "component carries both '" + at + "' and its legacy bare form '" + bare + "'");
      continue;
    }
    if (subject != &renamed) {
      renamed = component;
      subject = &renamed;
    }
    Json v = renamed[bare];
    renamed.erase(bare);
    renamed.set(at, std::move(v));
  }

  Json all = Json::array({Json::object({{"$ref", Json("common_types.json#/$defs/ComponentCommon")}})});
  const SchemaNode schema = api.schema_node();
  if (schema) {
    const SchemaNode closed = schema["additionalProperties"];
    if (closed && !(closed.is_bool() && closed.as_bool())) {
      // A closed component schema still admits the envelope keys, which the
      // other SDKs strip before checking the component schema.
      Json patched = schema.to_json();
      Json p = patched["properties"].is_object() ? patched["properties"] : Json::object();
      for (const char* k : {"id", "component", "@catalogId", "@metadata", "@accessibility", "@checks"}) {
        if (!p.contains(k)) p.set(k, Json(true));
      }
      patched.set("properties", std::move(p));
      all.push_back(std::move(patched));
    } else {
      all.push_back(Json::object({{"$ref", Json("catalog.json#/components/" + escape_pointer_token(api.name))}}));
    }
  }
  all.push_back(Json::object(
      {{"properties", Json::object({{"component", Json::object({{"type", Json("string")}})}})},
       {"required", Json::array({Json("component")})}}));
  Json wrapper = Json::object({{"allOf", std::move(all)}});
  // A component added in code without a schema declares nothing to close.
  if (!options.allow_unknown_properties && schema) {
    wrapper.set("unevaluatedProperties", Json(false));
  }
  c.schema_check(wrapper, *subject, "");

  const bool in_default = options.catalog_is_default.has_value() ? *options.catalog_is_default
                                                                  : !subject->contains("@catalogId");
  std::string path;
  c.walk(*subject, path, true, in_default, 0);
  return finish(c.issues, issues, what);
}

Status validate_function_call(const Catalog& catalog, const Json& call,
                              std::vector<ValidationIssue>* issues,
                              const ComponentValidationOptions& options) {
  if (!catalog.is_v1()) return unsupported_version(catalog);
  Checker c(catalog, options);
  const Json& name = call["@call"];
  const std::string what = "Function call '" + (name.is_string() ? name.as_string() : std::string("?")) + "' is invalid";
  if (!call.is_object() || !name.is_string()) {
    c.issue("", "required", "a function call must be an object with a string '@call'");
    return finish(c.issues, issues, what);
  }
  const Json* cat = call.find("catalogId");
  if (cat && !cat->is_string()) {
    c.issue("/catalogId", "type", "'catalogId' must be a string");
  } else if (!name.as_string().empty() && name.as_string()[0] == '@') {
    c.nested_call(name.as_string(), call, "", true);
  } else {
    c.full_call_check(name.as_string(), call, "");
  }
  std::string path = "/args";
  if (call["args"].is_container()) c.walk(call["args"], path, false, options.catalog_is_default.value_or(true), 0);
  return finish(c.issues, issues, what);
}

Status validate_common_type(std::string_view protocol_version, std::string_view definition,
                            const Json& value, std::vector<ValidationIssue>* issues) {
  const std::string version = normalize_protocol_version(protocol_version);
  auto common = spec::common_types(version);
  if (!common) {
    return make_error(ErrorKind::Generic, "No common types for protocol version '" +
                                              std::string(protocol_version) + "'");
  }
  if (!(*common)["$defs"].contains(definition)) {
    return make_error(ErrorKind::Generic, "'" + std::string(definition) +
                                              "' is not a common types definition");
  }
  SchemaRegistry reg;
  reg.add("common_types.json", common);
  reg.add("catalog.json", stand_in_catalog());
  return validate_ref(reg, "common_types.json#/$defs/" + escape_pointer_token(definition), value, issues);
}

}  // namespace a2ui

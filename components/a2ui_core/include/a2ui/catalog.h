// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Catalog layer (blueprint section 3.A): component and function declarations
// loaded from catalog JSON, plus the function implementations a renderer
// binds to them.
//
// CONTRACT: the members marked [core] are used by the state, processing and
// resolution layers. They can be extended but must keep their meaning.

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "a2ui/error.h"
#include "a2ui/json.h"

namespace a2ui {

enum class AllowedCallers : uint8_t { RendererOnly, AgentOnly, RendererOrAgent };

/**
 * What a function implementation can reach while it runs. DataContext
 * implements this; functions never see the DataContext type directly. [core]
 */
class FunctionContext {
 public:
  virtual ~FunctionContext() = default;
  /** Absolute data path of the evaluation scope ("/" at the root). */
  virtual const std::string& scope_path() const = 0;
  /** Index inside a template repeater, if any (for @index). */
  virtual std::optional<long> scope_index() const = 0;
  /** True while running inside an active user gesture. */
  virtual bool user_activated() const = 0;
  /**
   * Resolves a dynamic value in this scope: literals pass through,
   * {"@path"} reads the data model, {"@call"} evaluates a function.
   */
  virtual Result<Json> resolve(const Json& dynamic_value) = 0;
  /** The catalog the running function resolved to (formatString inherits it). */
  virtual const std::string& active_catalog_id() const = 0;
  /** Host hook for openUrl. Returns an error if the host refuses or has none. */
  virtual Status open_url(const std::string& url) = 0;
};

/** A function implementation: resolved args in, value out. */
using FunctionFn = std::function<Result<Json>(const Json& args, FunctionContext& ctx)>;

struct FunctionApi {  // [core]
  std::string name;
  /** "string", "number", "boolean", "array", "object", "any" or "void". */
  std::string return_type = "any";
  /** JSON Schema for the `args` object. */
  Json args_schema;
  AllowedCallers allowed_callers = AllowedCallers::RendererOnly;
  bool requires_user_activation = false;
  Json description;
};

struct FunctionDecl {  // [core]
  FunctionApi api;
  /** Empty when the renderer has no local implementation (calls route to the agent). */
  FunctionFn impl;
  bool has_local_impl() const { return static_cast<bool>(impl); }
};

/** How a component property participates in binding and the tree. [core] */
enum class PropertyKind : uint8_t {
  Static,          // Plain JSON, passed through.
  DynamicString,   // common_types DynamicString
  DynamicNumber,
  DynamicBoolean,
  DynamicStringList,
  DynamicValue,    // DynamicValue / any dynamic
  ComponentId,     // A single child reference.
  ChildList,       // Static list or template.
  Action,
  Checks,          // List of CheckRule.
};

struct ComponentApi {  // [core]
  std::string name;
  /** The component's JSON Schema as declared in the catalog. */
  Json schema;
  /** Classification of each top-level property, derived from schema $refs. */
  std::map<std::string, PropertyKind> property_kinds;
  /** Properties the schema marks required. */
  std::vector<std::string> required;
  /** Composition constraints; absent means unconstrained. */
  std::optional<std::vector<std::string>> allowed_parents;
  std::optional<std::vector<std::string>> allowed_children;
  /** ComponentDefinition.metadata (v1.0). */
  Json metadata;

  PropertyKind kind_of(std::string_view prop) const {
    auto it = property_kinds.find(std::string(prop));
    return it == property_kinds.end() ? PropertyKind::Static : it->second;
  }
};

class Catalog {
 public:
  Catalog() = default;

  const std::string& id() const { return id_; }  // [core]
  /** Normalized protocol version: "1.0", "0.9" or "0.8". [core] */
  const std::string& protocol_version() const { return protocol_version_; }

  const ComponentApi* find_component(std::string_view name) const;  // [core]
  const FunctionDecl* find_function(std::string_view name) const;   // [core]
  const std::vector<ComponentApi>& components() const { return components_; }
  const std::vector<FunctionDecl>& functions() const { return functions_; }

  /** Binds (or replaces) the implementation of a declared function. [core] */
  Status bind_function(std::string_view name, FunctionFn impl);
  /** Adds a function declaration with an implementation (e.g. a test helper). */
  void add_function(FunctionDecl decl);
  void add_component(ComponentApi api);

  void set_id(std::string id) { id_ = std::move(id); }
  void set_protocol_version(std::string v) { protocol_version_ = std::move(v); }

  /** The catalog document this was loaded from (for $ref resolution and to_json). */
  const Json& source() const { return source_; }
  void set_source(Json j) { source_ = std::move(j); }

 private:
  std::string id_;
  std::string protocol_version_ = "1.0";
  std::vector<ComponentApi> components_;
  std::vector<FunctionDecl> functions_;
  Json source_;
};

using CatalogPtr = std::shared_ptr<Catalog>;

}  // namespace a2ui

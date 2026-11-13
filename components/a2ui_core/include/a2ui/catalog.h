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
#include "a2ui/schema.h"

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
  /**
   * "string", "number", "boolean", "array", "object", "any" or "void"
   * (catalogs may also declare "validationResult").
   */
  std::string return_type = "any";
  /** JSON Schema for the `args` object. */
  Json args_schema;
  /**
   * The function's whole definition as the catalog declares it (validates a
   * complete call: `@call`, `args`). Undefined for functions added in code.
   */
  Json schema;
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
  /**
   * Properties the schema marks required, including those of its `allOf`
   * members and mixins. The envelope keys `id` and `component` are left out.
   */
  std::vector<std::string> required;
  /** Composition constraints; absent means unconstrained. */
  std::optional<std::vector<std::string>> allowed_parents;
  std::optional<std::vector<std::string>> allowed_children;
  /** ComponentDefinition.metadata (v1.0); undefined when the definition has none. */
  Json metadata;

  /**
   * Where the component references other components, read from schema
   * `$ref`s ending in /ComponentId, /Child or /ChildList (the reference
   * map of the other SDKs). Filled by Catalog::from_json and
   * Catalog::add_component.
   */
  struct ChildRefs {
    /** Properties holding one component id. */
    std::vector<std::string> single;
    /** Properties holding a ChildList (id array or template). */
    std::vector<std::string> list;
    /** Array properties whose items are objects with child slots (Tabs.tabs[].child). */
    std::map<std::string, std::vector<std::string>> nested;
    bool empty() const { return single.empty() && list.empty(); }
  };
  ChildRefs child_refs;

  /**
   * Which of the bare envelope names (metadata, catalogId, accessibility,
   * checks) the component declares as properties of its own rather than as
   * the standard ComponentCommon / Checkable mixin (any allOf/anyOf/oneOf
   * branch counts). On a v1.0 surface a bare key that is not listed here is
   * the legacy spelling of its `@` envelope key.
   */
  std::vector<std::string> custom_envelope_properties;
  /** Whether the component composes Checkable (accepts `@checks`). */
  bool checkable = false;

  bool declares_custom(std::string_view bare_key) const {
    for (const auto& k : custom_envelope_properties) {
      if (k == bare_key) return true;
    }
    return false;
  }

  PropertyKind kind_of(std::string_view prop) const {
    auto it = property_kinds.find(std::string(prop));
    return it == property_kinds.end() ? PropertyKind::Static : it->second;
  }
};

class Catalog;
using CatalogPtr = std::shared_ptr<Catalog>;

/**
 * Normalizes a protocol version spelling ("v0.9", "0.9.1", "1.0", "v1.0.0")
 * to "1.0", "0.9" or "0.8". Returns an empty string for anything else.
 */
std::string normalize_protocol_version(std::string_view version);

/** One entry of a `Catalog::derive` list: keep `name`, or add `definition` under it. */
struct DeriveEntry {
  std::string name;
  /** Undefined to keep the source catalog's entry `name`; else a new definition. */
  Json definition;
};

class Catalog {
 public:
  Catalog() = default;

  /**
   * Loads a catalog document (specification catalog_definition.json shape).
   *
   * `catalog_id` and `protocol_version`, when non-empty, take precedence
   * over the document's `catalogId` and `protocolVersion`. A document with
   * neither version targets "0.9", the specification default.
   *
   * Errors (ErrorKind::Catalog): not an object, no catalog id, an unknown
   * version, malformed components/functions, and for v1.0: an identifier
   * that is not UAX #31 (component, property, function, argument), the
   * reserved component name `Surface`, a `$ref` to a document other than
   * the catalog itself or common_types.json, and requiresUserActivation
   * with allowedCallers other than rendererOnly. Invalid component
   * `metadata` is ErrorKind::Validation.
   *
   * Entries a `$defs.anyComponent` / `$defs.anyFunction` oneOf union leaves
   * out are not loaded (but are kept for to_json).
   *
   * Memory: component and function definitions are moved out of the
   * document into the ComponentApi / FunctionApi, not copied; `source()`
   * keeps the rest of the document.
   */
  static Result<CatalogPtr> from_json(const Json& document, std::string_view catalog_id = {},
                                      std::string_view protocol_version = {});

  /** The catalog document (unbundled: external references kept as written). */
  Json to_json() const;

  /**
   * A self-contained JSON Schema for the catalog: to_json() with every
   * common_types.json definition it reaches copied into `$defs` and every
   * reference rewritten to a local "#/..." pointer.
   */
  Json validation_schema() const;

  /**
   * A catalog with a new id built from entries of this one (see DeriveEntry).
   * A null list keeps every entry of that kind. Keeps the protocol version,
   * theme and `$defs`; drops `$id`, title, description and instructions
   * (replaced by `instructions` when given). An unknown name is
   * ErrorKind::Catalog.
   */
  Result<CatalogPtr> derive(std::string_view id, const std::vector<DeriveEntry>* components,
                            const std::vector<DeriveEntry>* functions,
                            const std::string* instructions = nullptr) const;

  /**
   * Schema documents for validating against this catalog: "catalog.json"
   * (this catalog; "#/components/<name>" and "#/functions/<name>" reach the
   * loaded definitions) and "common_types.json" of its protocol version.
   * The registry refers to this catalog, which must outlive it.
   */
  SchemaRegistry schema_registry() const;

  /** The version as declared or given, before normalization (e.g. "v0.9"); may be empty. */
  const std::string& declared_protocol_version() const { return declared_protocol_version_; }
  bool is_v1() const { return protocol_version_ == "1.0"; }

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

  /**
   * The catalog document this was loaded from, without the bodies of the
   * loaded components and functions (those live in components() and
   * functions()). Resolve references through schema_registry().
   */
  const Json& source() const { return source_; }
  void set_source(Json j) { source_ = std::move(j); }

 private:
  const Json* resolve_catalog_pointer(std::string_view pointer) const;

  std::string id_;
  std::string protocol_version_ = "1.0";
  std::string declared_protocol_version_;
  std::vector<ComponentApi> components_;
  std::vector<FunctionDecl> functions_;
  Json source_;
};

}  // namespace a2ui

// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Single-catalog payload validation (blueprint "Validation Layer",
// "Catalog Scope" and "Component Envelope Keys"): one component, or one
// function call, checked against one catalog. The message processor
// resolves which catalog an item belongs to and calls these.
//
// v1.0 only: a catalog of another protocol version is reported as an
// ErrorKind::Generic "unsupported" error.

#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "a2ui/catalog.h"
#include "a2ui/error.h"
#include "a2ui/json.h"
#include "a2ui/schema.h"

namespace a2ui {

struct ComponentValidationOptions {
  /** Accept properties the component schema does not declare (allowUnknownElements). */
  bool allow_unknown_properties = false;
  /** Accept calls to functions the catalog does not declare (allowUnknownElements). */
  bool allow_unknown_functions = false;
  /**
   * Whether the component's catalog is the surface default catalog, which
   * decides where a nested call that names no `catalogId` runs. When unset,
   * a component without `@catalogId` is taken to be in the default catalog.
   */
  std::optional<bool> catalog_is_default;
};

/**
 * Validates one component instance against `api` (a component of `catalog`)
 * wrapped in the v1.0 component envelope:
 *
 *   allOf: [common_types ComponentCommon, the component schema,
 *           {component: <type name>}], unevaluatedProperties: false
 *
 * Inside the component schema a FunctionCall is checked only as an envelope
 * (`@call`, `catalogId`, `args`). Each nested call is then checked on its
 * own: fully against `catalog` when it runs there (it names this catalog,
 * or names none and the component is in the surface default catalog);
 * otherwise only its function and argument identifiers. `@`-prefixed calls
 * are system functions: they must not name a catalogId and only `@index`
 * exists. Single-`@` keys other than `@path` / `@call` in nested objects
 * are rejected (escape them as "@@"). During the v1.0 transition a bare
 * `catalogId`, `accessibility`, `metadata` or `checks` the schema does not
 * declare is read as the `@` envelope key.
 *
 * Returns an ErrorKind::Validation error (code "VALIDATION_FAILED") whose
 * message names the first issue and whose `details` lists all of them;
 * issue paths are JSON Pointers into `component`. Issues are also appended
 * to `*issues` when given.
 */
Status validate_component_properties(const Catalog& catalog, const ComponentApi& api,
                                     const Json& component,
                                     std::vector<ValidationIssue>* issues = nullptr,
                                     const ComponentValidationOptions& options = {});

/**
 * Fully checks one function call ({"@call", "args", "catalogId"?}) against
 * `catalog`: the function is declared (unless allow_unknown_functions), the
 * renderer may call it (not agentOnly), and the call matches the function's
 * definition. Calls nested in its arguments are checked too, as in
 * validate_component_properties.
 */
Status validate_function_call(const Catalog& catalog, const Json& call,
                              std::vector<ValidationIssue>* issues = nullptr,
                              const ComponentValidationOptions& options = {});

/**
 * Validates `value` against a common types definition (a `$defs` name of
 * common_types.json) of a protocol version ("1.0"). `catalog.json`
 * references resolve to a permissive stand-in catalog, so a FunctionCall is
 * checked as an envelope. An unknown version or definition is
 * ErrorKind::Generic.
 */
Status validate_common_type(std::string_view protocol_version, std::string_view definition,
                            const Json& value, std::vector<ValidationIssue>* issues = nullptr);

}  // namespace a2ui

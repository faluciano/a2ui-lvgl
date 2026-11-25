// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// A JSON Schema (draft 2020-12) interpreter for the subset that the A2UI
// specification and catalogs use. Schemas are interpreted in place, from
// their parsed Json or from a PackedDocument in flash (a2ui/schema_node.h);
// nothing is compiled or copied.
//
// Supported keywords:
//   $ref (local, cross-document by name/$id/basename), $defs, type, enum,
//   const, properties, patternProperties, additionalProperties,
//   unevaluatedProperties, required, propertyNames, minProperties,
//   maxProperties, dependentRequired, dependentSchemas, items, prefixItems,
//   unevaluatedItems, contains, minContains, maxContains, minItems,
//   maxItems, uniqueItems, minimum, maximum, exclusiveMinimum,
//   exclusiveMaximum, multipleOf, minLength, maxLength, pattern, allOf,
//   anyOf, oneOf, not, if/then/else.
// Ignored (annotations only): format, title, description, default,
//   examples, discriminator, $schema, $id, $comment, and A2UI extension
//   keywords such as returnType or allowedParents.
//
// `pattern` uses a small backtracking matcher (schema_pattern.cpp) that
// covers ECMAScript syntax used by schemas: anchors, classes, escapes,
// groups, alternation, greedy and lazy quantifiers and the Unicode
// properties \p{XID_Start}, \p{XID_Continue}, \p{L}, \p{N}. Matching has a
// step budget, so a pathological pattern fails closed instead of hanging.
//
// The reference to the 2020-12 meta-schema
// ("https://json-schema.org/draft/2020-12/schema") is treated as `true`:
// A2UI uses it to say "any schema", and the meta-schema is not bundled.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "a2ui/error.h"
#include "a2ui/json.h"
#include "a2ui/ref.h"
#include "a2ui/schema_node.h"

namespace a2ui {

/** One schema violation. */
struct ValidationIssue {
  /** JSON Pointer to the offending value inside the validated instance ("" = root). */
  std::string instance_path;
  /** The schema keyword that failed (e.g. "required", "oneOf", "$ref"). */
  std::string keyword;
  std::string message;
};

/** Renders issues as a Json array of {path, keyword, message} objects (for Error::details). */
Json issues_to_json(const std::vector<ValidationIssue>& issues);

/**
 * Documents a validation can reach through `$ref`. A document is registered
 * under a short name ("common_types.json", "catalog.json") and is also found
 * by its `$id`, or by the last path segment of a reference's document part,
 * so "https://a2ui.org/specification/v1_0/common_types.json" and
 * "common_types.json" reach the same document.
 *
 * The registry is cheap to copy: documents are shared, never copied.
 */
class SchemaRegistry {
 public:
  /**
   * Resolves a JSON Pointer inside a document. Used for documents whose
   * parts live outside one Json tree (a loaded catalog keeps component
   * schemas next to the document rather than inside it). Returns an absent
   * node (or a null `const Json*`) when the pointer does not resolve.
   */
  using PointerResolver = std::function<SchemaNode(std::string_view pointer)>;

  /** Registers (or replaces) a document. Its `$id`, if any, is an alias. */
  void add(std::string name, Ref<const Json> document);
  /** Registers a document whose pointers go through `resolver`. */
  void add(std::string name, Ref<const Json> document, PointerResolver resolver);
  /** Registers a packed document, which must outlive the registry (flash data does). */
  void add(std::string name, const PackedDocument& document);
  /**
   * Makes a reference resolve to `schema` instead of its target. The key is
   * "<document name>#<pointer>", e.g. "common_types.json#/$defs/FunctionCall".
   * The override schema is evaluated in the context of that document.
   */
  void add_override(std::string ref, Ref<const Json> schema);

  /** Index of the document a reference's document part names, or -1. */
  int find_document(std::string_view doc_part) const;
  /** Resolves `pointer` in document `doc`. Absent if it does not exist. */
  SchemaNode resolve_pointer(int doc, std::string_view pointer) const;
  const std::string& document_name(int doc) const { return docs_[static_cast<size_t>(doc)].name; }
  const Json* override_for(int doc, std::string_view pointer) const;

 private:
  struct Doc {
    std::string name;
    std::string id;
    Ref<const Json> json;
    const PackedDocument* packed = nullptr;
    PointerResolver resolver;
  };
  struct Override {
    std::string key;
    Ref<const Json> schema;
  };
  std::vector<Doc> docs_;
  std::vector<Override> overrides_;
};

struct SchemaValidateOptions {
  /**
   * Maximum nesting of schema evaluation (bounds $ref recursion and deep
   * instances). Each level is one recursive call of a few hundred bytes of
   * stack; validating a basic-catalog component needs about 30 levels, so
   * run validation on a task with at least 16 KB of stack.
   */
  int max_depth = 64;
  /** Stop collecting after this many issues (validation still fails). */
  size_t max_issues = 16;
};

/**
 * Validates `instance` against `schema`, which is evaluated as part of the
 * registry document `doc_name` (so "#/..." references resolve there). The
 * schema does not need to live inside that document. A `const Json&`
 * converts to SchemaNode.
 *
 * Returns ok when valid. Otherwise an ErrorKind::Validation error whose
 * message names the first issue and whose `details` lists every issue; the
 * issues are also appended to `*issues` when given. Exceeding `max_depth`
 * yields ErrorKind::Recursion; an unresolvable `$ref` is a Validation issue
 * with keyword "$ref".
 */
Status validate_schema(const SchemaRegistry& registry, std::string_view doc_name,
                       SchemaNode schema, const Json& instance,
                       std::vector<ValidationIssue>* issues = nullptr,
                       const SchemaValidateOptions& options = {});

/**
 * Validates against a reference such as "common_types.json#/$defs/DynamicString".
 * An unresolvable reference is an ErrorKind::Generic error.
 */
Status validate_ref(const SchemaRegistry& registry, std::string_view ref, const Json& instance,
                    std::vector<ValidationIssue>* issues = nullptr,
                    const SchemaValidateOptions& options = {});

/** Splits a reference into document part and fragment pointer ("" when absent). */
void split_ref(std::string_view ref, std::string_view* doc_part, std::string_view* pointer);

/** Resolves an RFC 6901 pointer (with URI %-decoding) inside `root`. */
const Json* resolve_json_pointer(const Json& root, std::string_view pointer);

/** Escapes one RFC 6901 reference token ("~" -> "~0", "/" -> "~1"). */
std::string escape_pointer_token(std::string_view token);

namespace schema_pattern {

/**
 * ECMAScript-style search (RegExp.prototype.test) of `pattern` in UTF-8
 * `text`. Returns false for patterns it cannot parse, and sets *ok (when
 * given) to false for a parse error or an exhausted step budget.
 */
bool search(std::string_view pattern, std::string_view text, bool* ok = nullptr);

}  // namespace schema_pattern

}  // namespace a2ui

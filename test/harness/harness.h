// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Conformance harness plumbing shared by every suite runner.
// See third_party/a2ui/conformance/conformance_harness_implementation_guide.md.
//
// Each upstream YAML suite is converted to JSON at build time. A case is a
// JSON object with an `action`; runners register a handler per action name
// with HARNESS_ACTION and return an Outcome.

#pragma once

#include <string>

#include "a2ui/error.h"
#include "a2ui/json.h"

namespace harness {

using a2ui::Json;

struct Outcome {
  enum Kind { kPass, kFail, kUnsupported };
  Kind kind = kPass;
  std::string detail;

  static Outcome pass() { return {}; }
  static Outcome fail(std::string why) { return {kFail, std::move(why)}; }
  static Outcome unsupported(std::string why) { return {kUnsupported, std::move(why)}; }
  bool passed() const { return kind == kPass; }
};

struct Context {
  std::string repo_root;         // Upstream repo root (third_party/a2ui).
  std::string conformance_root;  // <repo_root>/conformance
  bool verbose = false;

  std::string repo_path(const std::string& rel) const { return repo_root + "/" + rel; }
  std::string conformance_path(const std::string& rel) const {
    return conformance_root + "/" + rel;
  }
};

using ActionFn = Outcome (*)(const Json& test_case, const Context& ctx);

struct ActionRegistrar {
  ActionRegistrar(const char* action, ActionFn fn);
};

/** Defines and registers a handler for one `action` value. */
#define HARNESS_ACTION(action_name, fn_name)                                       \
  static harness::Outcome fn_name(const harness::Json& test_case,                 \
                                  const harness::Context& ctx);                   \
  static harness::ActionRegistrar fn_name##_registrar(action_name, &fn_name);     \
  static harness::Outcome fn_name(const harness::Json& test_case,                 \
                                  const harness::Context& ctx)

// ---------------------------------------------------------------------------
// Helpers

/** Reads and parses a JSON file; on failure returns undefined and sets *error. */
Json load_json_file(const std::string& path, std::string* error = nullptr);
std::string read_file(const std::string& path, bool* ok = nullptr);

/** Returns `expectError` or `expect_error` (cores use both spellings), or nullptr. */
const Json* find_expect_error(const Json& node);

/**
 * Checks an Error against an ExpectError object: `category` (with the
 * blueprint's inheritance), `message` (regex search), and optional
 * `code`, `surfaceId`, `path`, `functionCallId` fields when present.
 */
Outcome check_error(const a2ui::Error& error, const Json& expect);

/** Outcome for "expected an error but the call succeeded". */
Outcome missing_error(const Json& expect);

/**
 * Deep comparison of `expected` against `actual`. A `{"$match": "any"}` node
 * in `expected` matches any defined non-null value. Returns empty when they
 * match, otherwise a short description of the first mismatch with its path.
 */
std::string json_mismatch(const Json& expected, const Json& actual, const std::string& path = "");

/** True when the case targets a protocol version before v1.0 (unsupported here). */
bool targets_pre_v1(const Json& test_case);

/** Regex search used for `message` expectations. */
bool regex_search(const std::string& pattern, const std::string& text);

}  // namespace harness

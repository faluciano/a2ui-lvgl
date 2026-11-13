// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0

#include "a2ui/metadata.h"

#include "a2ui/identifier.h"

namespace a2ui {

Status validate_metadata(const Json& metadata, const std::string& context) {
  if (metadata.is_nullish()) return ok_status();
  if (!metadata.is_object()) {
    return make_error(ErrorKind::Validation,
                      "Expected " + context + " to be an object, got " + metadata.dump());
  }
  for (const auto& m : metadata.as_object()) {
    if (m.first != "extensions") {
      return make_error(ErrorKind::Validation, "Unexpected property '" + m.first + "' in " +
                                                   context + "; only 'extensions' is allowed.");
    }
    if (!m.second.is_object()) {
      return make_error(ErrorKind::Validation,
                        "Expected " + context + ".extensions to be an object.");
    }
    for (const auto& e : m.second.as_object()) {
      if (!is_uax31_identifier(e.first)) {
        return make_error(ErrorKind::Validation, "Invalid UAX #31 extension identifier '" +
                                                     e.first + "' in " + context + ".extensions.");
      }
    }
  }
  return ok_status();
}

Json normalize_metadata(const Json& metadata) {
  if (!metadata.is_object()) return Json();
  const Json& ext = metadata["extensions"];
  if (!ext.is_object() || ext.empty()) return Json();
  return metadata;
}

Result<bool> metadata_prepare(const Json& current, const Json& next, const std::string& context,
                              Json* out) {
  A2UI_RETURN_IF_ERROR(validate_metadata(next, context));
  *out = normalize_metadata(next);
  return !(*out == current) || out->type() != current.type();
}

Result<Json> metadata_with_extension(const Json& current, const std::string& key, Json value,
                                     const std::string& context) {
  if (!is_uax31_identifier(key)) {
    return Error(make_error(ErrorKind::Validation, "Invalid UAX #31 extension identifier '" + key +
                                                       "' in " + context + ".extensions."));
  }
  Json ext = current["extensions"].is_object() ? current["extensions"] : Json::object();
  ext.set(key, value.is_undefined() ? Json(nullptr) : std::move(value));
  return Json::object({{"extensions", std::move(ext)}});
}

Json metadata_without_extension(const Json& current, const std::string& key) {
  if (!current["extensions"].is_object() || !current["extensions"].contains(key)) return current;
  Json ext = current["extensions"];
  ext.erase(key);
  if (ext.empty()) return Json();
  return Json::object({{"extensions", std::move(ext)}});
}

}  // namespace a2ui

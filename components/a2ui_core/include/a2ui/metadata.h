// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// The v1.0 metadata envelope ({"extensions": {<UAX #31 key>: value}}) and
// helpers for the MetadataHolder contract that SurfaceModel and
// ComponentModel implement (blueprint section E, "MetadataHolder").

#pragma once

#include <string>

#include "a2ui/error.h"
#include "a2ui/json.h"

namespace a2ui {

/**
 * Checks a metadata envelope: an object holding only `extensions`, an
 * object whose keys are UAX #31 identifiers. Undefined and null pass (no
 * metadata). Fails with ErrorKind::Validation; `context` names the owner in
 * the message ("surface 's1' metadata").
 */
Status validate_metadata(const Json& metadata, const std::string& context);

/** A copy of `metadata` with an empty or missing `extensions` collapsed to undefined. */
Json normalize_metadata(const Json& metadata);

/**
 * The MetadataHolder write rules as helpers for the holders (SurfaceModel,
 * ComponentModel): validate, normalize, and report whether `next` differs
 * from `current`. On success `*out` holds the value to store.
 */
Result<bool> metadata_prepare(const Json& current, const Json& next, const std::string& context,
                              Json* out);
/** The envelope with extension `key` set (null is a stored value), validated. */
Result<Json> metadata_with_extension(const Json& current, const std::string& key, Json value,
                                     const std::string& context);
/** The envelope without extension `key` (undefined once empty). */
Json metadata_without_extension(const Json& current, const std::string& key);

}  // namespace a2ui

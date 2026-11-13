// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// DataModel: the per-surface JSON document addressed by JSON Pointer.
// Implements the rules in blueprints/modules/a2ui_core.blueprint.md (DataModel):
// auto-vivification, forbidden segments, bubble/cascade notification that
// fires only for paths whose value actually changed, null-write-as-delete,
// root reset, and value ownership (every write stores its own copy).

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "a2ui/error.h"
#include "a2ui/events.h"
#include "a2ui/json.h"

namespace a2ui {

/** JSON Pointer helpers. Paths are normalized: empty segments are dropped. */
namespace path {

/** Splits and unescapes (~1 -> '/', ~0 -> '~') an absolute or relative pointer. */
std::vector<std::string> split(std::string_view pointer);
/** Joins segments into a normalized absolute pointer ("/" for none). */
std::string join(const std::vector<std::string>& segments);
/** Escapes one segment for use in a pointer. */
std::string escape_segment(std::string_view segment);
/** Normalizes an absolute pointer ("//a///b/" -> "/a/b", "" -> "/"). */
std::string normalize(std::string_view pointer);
/**
 * Resolves `p` against `context` (an absolute pointer). Absolute paths ignore
 * the context; "" and "." name the context itself.
 */
std::string resolve(std::string_view p, std::string_view context = "/");
/** True if `p` is an absolute pointer. */
inline bool is_absolute(std::string_view p) { return !p.empty() && p[0] == '/'; }
/** True if normalized `ancestor` equals or contains normalized `p`. */
bool is_same_or_descendant(std::string_view p, std::string_view ancestor);
/** Canonical array index ("0", "12"; not "01", "-1"), else -1. */
long parse_index(std::string_view segment);

}  // namespace path

class DataModel {
 public:
  /** The largest array index a write may create (matches the other SDKs). */
  static constexpr long kMaxArrayIndex = 10000;

  explicit DataModel(Json initial = Json::object());
  DataModel(const DataModel&) = delete;
  DataModel& operator=(const DataModel&) = delete;

  /**
   * Reads the value at an absolute pointer. Returns nullptr when absent
   * (including sparse holes). Fails only for forbidden segments.
   */
  Result<const Json*> get(std::string_view pointer) const;

  /** Convenience: the value or undefined (forbidden paths read as undefined). */
  Json value_at(std::string_view pointer) const;

  /**
   * Writes a copy of `value` at `pointer`. Writing null or undefined removes
   * the value (or is a no-op when nothing is there).
   */
  Status set(std::string_view pointer, Json value);

  /** Removes the value at `pointer` (same as set(pointer, undefined)). */
  Status remove(std::string_view pointer) { return set(pointer, Json()); }

  using Listener = std::function<void(const Json& new_value)>;
  /**
   * Observes a path. The listener runs after any write that changes the
   * value at that path: exact writes, writes below it (bubble) and writes
   * above it that change it (cascade).
   */
  [[nodiscard]] Subscription subscribe(std::string_view pointer, Listener listener);

  /** Number of active path observers (for leak tests). */
  size_t observer_count() const;

  /** Fires after every successful change, with the normalized written path. */
  const EventSource<std::string>& on_changed() const { return on_changed_; }

  /** Drops every observer. The document stays readable and writable. */
  void dispose();

  const Json& root() const { return root_; }

 private:
  struct Observer {
    std::string path;                  // normalized
    std::vector<std::string> segments;
    Listener listener;
    bool alive = true;
  };

  void notify(const std::vector<std::string>& written, const Json& old_value, bool changed);

  Json root_;
  std::shared_ptr<std::vector<std::shared_ptr<Observer>>> observers_;
  EventSource<std::string> on_changed_;
};

}  // namespace a2ui

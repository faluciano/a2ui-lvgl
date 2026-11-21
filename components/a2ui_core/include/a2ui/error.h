// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Error values for the A2UI core. The library is built without C++
// exceptions, so the blueprint's exception hierarchy (A2uiError and its
// subclasses) is modelled as an `ErrorKind` with the same inheritance:
// IntegrityError and RecursionError are both ValidationErrors.

#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "a2ui/json.h"

namespace a2ui {

enum class ErrorKind : uint8_t {
  Generic,     // A2uiError
  Validation,  // A2uiValidationError
  Integrity,   // A2uiIntegrityError : A2uiValidationError
  Recursion,   // A2uiRecursionError : A2uiValidationError
  Catalog,     // A2uiCatalogError
  Rpc,         // A2uiRpcError
  Data,        // A2uiDataError
  State,       // A2uiStateError
  Parse,       // A2uiParseError
  Expression,  // A2uiExpressionError
};

/** The class name without the `A2ui` prefix, as used by conformance `category`. */
const char* error_kind_name(ErrorKind kind);

struct Error {
  ErrorKind kind = ErrorKind::Generic;
  std::string message;
  /** Wire error code for outbound `error` envelopes (e.g. "VALIDATION_FAILED"). */
  std::string code;
  std::string surface_id;
  std::string path;
  std::string function_call_id;
  /** Optional structured details (e.g. a list of nested failures). */
  Json details;

  /** True if this error is `k` or a subclass of it (Integrity/Recursion are Validation). */
  bool is(ErrorKind k) const {
    if (kind == k || k == ErrorKind::Generic) return true;
    return k == ErrorKind::Validation &&
           (kind == ErrorKind::Integrity || kind == ErrorKind::Recursion);
  }
};

inline Error make_error(ErrorKind kind, std::string message, std::string code = {}) {
  Error e;
  e.kind = kind;
  e.message = std::move(message);
  e.code = std::move(code);
  return e;
}

/** A value or an Error. Deliberately minimal; no exceptions are thrown. */
template <class T>
class Result {
 public:
  Result(T value) : ok_(true), value_(std::move(value)) {}  // NOLINT
  Result(Error error) : ok_(false), error_(std::move(error)) {}  // NOLINT

  bool ok() const { return ok_; }
  explicit operator bool() const { return ok_; }
  const T& value() const& { return value_; }
  T& value() & { return value_; }
  T&& value() && { return std::move(value_); }
  const Error& error() const { return error_; }
  Error&& take_error() { return std::move(error_); }

 private:
  bool ok_;
  T value_{};
  Error error_;
};

template <>
class Result<void> {
 public:
  Result() = default;
  Result(Error error) : ok_(false), error_(std::move(error)) {}  // NOLINT

  bool ok() const { return ok_; }
  explicit operator bool() const { return ok_; }
  const Error& error() const { return error_; }
  Error&& take_error() { return std::move(error_); }

 private:
  bool ok_ = true;
  Error error_;
};

using Status = Result<void>;

inline Status ok_status() { return Status(); }

}  // namespace a2ui

/** Propagates a failed Status/Result from an expression returning one. */
#define A2UI_RETURN_IF_ERROR(expr)              \
  do {                                          \
    auto a2ui_status_ = (expr);                 \
    if (!a2ui_status_.ok()) return a2ui_status_.take_error(); \
  } while (0)

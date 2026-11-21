// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0

#include "a2ui/error.h"
#include "a2ui/events.h"

namespace a2ui {

namespace detail {
BatchState& batch_state() {
  static BatchState state;
  return state;
}
}  // namespace detail

const char* error_kind_name(ErrorKind kind) {
  switch (kind) {
    case ErrorKind::Generic: return "Error";
    case ErrorKind::Validation: return "ValidationError";
    case ErrorKind::Integrity: return "IntegrityError";
    case ErrorKind::Recursion: return "RecursionError";
    case ErrorKind::Catalog: return "CatalogError";
    case ErrorKind::Rpc: return "RpcError";
    case ErrorKind::Data: return "DataError";
    case ErrorKind::State: return "StateError";
    case ErrorKind::Parse: return "ParseError";
    case ErrorKind::Expression: return "ExpressionError";
  }
  return "Error";
}

}  // namespace a2ui

// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Reference-counted pointers for a single-threaded library.
//
// The core and the LVGL adapter run on one task, so their shared ownership
// needs no atomics or locks. With libstdc++, Ref<T> is
// std::__shared_ptr<T, _S_single>: the same interface as std::shared_ptr
// (copy, reset, get, ->, comparisons, std::hash, static_pointer_cast,
// shared_from_this) with plain integer reference counts.
//
// Why not std::shared_ptr: ESP-IDF's toolchains build libstdc++ with the
// mutex lock policy. There, every control block embeds a pthread mutex that
// is created lazily, as a FreeRTOS semaphore in internal RAM, the first time
// a weak_ptr is promoted (weak_ptr::lock, shared_from_this). The mutex has
// no destructor, so the semaphore leaks when the control block dies.
//
// Other standard libraries (libc++, MSVC) have no single-threaded variant;
// there Ref<T> is std::shared_ptr<T>, which is correct, just not minimal.
//
// Do not share a Ref between tasks. Pass data to the library's task instead
// (see the threading notes in events.h).

#pragma once

#include <memory>
#include <utility>

namespace a2ui {

#if defined(__GLIBCXX__)

/** True when Ref<T> uses non-atomic reference counts. */
inline constexpr bool kRefIsSingleThreaded = true;

template <class T>
using Ref = std::__shared_ptr<T, __gnu_cxx::_S_single>;
template <class T>
using WeakRef = std::__weak_ptr<T, __gnu_cxx::_S_single>;
template <class T>
using EnableRefFromThis = std::__enable_shared_from_this<T, __gnu_cxx::_S_single>;

/** make_shared for Ref<T>: one allocation for the object and its counts. */
template <class T, class... Args>
Ref<T> make_ref(Args&&... args) {
  return std::__make_shared<T, __gnu_cxx::_S_single>(std::forward<Args>(args)...);
}

#else

inline constexpr bool kRefIsSingleThreaded = false;

template <class T>
using Ref = std::shared_ptr<T>;
template <class T>
using WeakRef = std::weak_ptr<T>;
template <class T>
using EnableRefFromThis = std::enable_shared_from_this<T>;

template <class T, class... Args>
Ref<T> make_ref(Args&&... args) {
  return std::make_shared<T>(std::forward<Args>(args)...);
}

#endif

}  // namespace a2ui


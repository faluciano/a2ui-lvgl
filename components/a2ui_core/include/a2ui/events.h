// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Observer primitives required by the core blueprint ("Observable Library"):
// - EventSource<Args...>: multicast discrete events.
// - Signal<T>: a stateful value with change notification.
// - Subscription: RAII handle; destroying or resetting it unsubscribes.
// - Batch: RAII scope that coalesces Signal notifications until it ends.
//
// Threading: like LVGL itself, the core is single-threaded. Call it from one
// task (the LVGL task on device) and marshal transport data onto that task.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace a2ui {

class Subscription {
 public:
  Subscription() = default;
  explicit Subscription(std::function<void()> on_unsubscribe)
      : on_unsubscribe_(std::move(on_unsubscribe)) {}
  Subscription(const Subscription&) = delete;
  Subscription& operator=(const Subscription&) = delete;
  Subscription(Subscription&& o) noexcept : on_unsubscribe_(std::move(o.on_unsubscribe_)) {
    o.on_unsubscribe_ = nullptr;
  }
  Subscription& operator=(Subscription&& o) noexcept {
    if (this != &o) {
      unsubscribe();
      on_unsubscribe_ = std::move(o.on_unsubscribe_);
      o.on_unsubscribe_ = nullptr;
    }
    return *this;
  }
  ~Subscription() { unsubscribe(); }

  void unsubscribe() {
    if (on_unsubscribe_) {
      auto fn = std::move(on_unsubscribe_);
      on_unsubscribe_ = nullptr;
      fn();
    }
  }
  /** Detaches the handle without unsubscribing (the listener lives as long as its source). */
  void release() { on_unsubscribe_ = nullptr; }
  bool active() const { return static_cast<bool>(on_unsubscribe_); }

 private:
  std::function<void()> on_unsubscribe_;
};

/** Owns a group of subscriptions and drops them together. */
class SubscriptionBag {
 public:
  void add(Subscription s) { subs_.push_back(std::move(s)); }
  void clear() {
    // Move out first: unsubscribing may re-enter and touch this bag.
    auto subs = std::move(subs_);
    subs_.clear();
    subs.clear();
  }
  size_t size() const { return subs_.size(); }
  ~SubscriptionBag() { clear(); }

 private:
  std::vector<Subscription> subs_;
};

template <class... Args>
class EventSource {
 public:
  using Listener = std::function<void(const Args&...)>;

  EventSource() : state_(std::make_shared<State>()) {}
  EventSource(const EventSource&) = delete;
  EventSource& operator=(const EventSource&) = delete;

  /** Registers `fn`; the returned handle unsubscribes when destroyed. */
  [[nodiscard]] Subscription subscribe(Listener fn) const {
    auto entry = std::make_shared<Entry>();
    entry->fn = std::move(fn);
    state_->entries.push_back(entry);
    std::weak_ptr<State> weak_state = state_;
    std::weak_ptr<Entry> weak_entry = entry;
    return Subscription([weak_state, weak_entry] {
      auto s = weak_state.lock();
      auto e = weak_entry.lock();
      if (!s || !e) return;
      e->alive = false;
      auto& v = s->entries;
      for (auto it = v.begin(); it != v.end(); ++it) {
        if (*it == e) {
          v.erase(it);
          break;
        }
      }
    });
  }

  /** Calls every listener. Listeners may (un)subscribe while being called. */
  void emit(const Args&... args) const {
    auto keep = state_;
    auto snapshot = keep->entries;  // shared_ptr copies; cheap.
    for (const auto& e : snapshot) {
      if (e->alive) e->fn(args...);
    }
  }

  size_t listener_count() const { return state_->entries.size(); }

  /** Drops every listener (used by dispose()). */
  void clear() const {
    for (auto& e : state_->entries) e->alive = false;
    state_->entries.clear();
  }

 private:
  struct Entry {
    Listener fn;
    bool alive = true;
  };
  struct State {
    std::vector<std::shared_ptr<Entry>> entries;
  };
  std::shared_ptr<State> state_;
};

namespace detail {
/** Notification queue shared by all signals while a Batch is open. */
struct BatchState {
  int depth = 0;
  std::vector<std::function<void()>> pending;
};
BatchState& batch_state();
}  // namespace detail

/** Defers Signal notifications until the outermost Batch ends. */
class Batch {
 public:
  Batch() { ++detail::batch_state().depth; }
  Batch(const Batch&) = delete;
  Batch& operator=(const Batch&) = delete;
  ~Batch() {
    auto& st = detail::batch_state();
    if (--st.depth > 0) return;
    // Flushing may open new batches and queue more work; drain until empty.
    while (!st.pending.empty()) {
      auto work = std::move(st.pending);
      st.pending.clear();
      ++st.depth;
      for (auto& fn : work) fn();
      --st.depth;
    }
  }
};

/**
 * A value that notifies subscribers when it changes. `set` compares with
 * operator== and skips notification for equal values. Inside a Batch,
 * listeners are called once at the end with the latest value.
 */
template <class T>
class Signal {
 public:
  using Listener = std::function<void(const T&)>;

  Signal() : state_(std::make_shared<State>()) {}
  explicit Signal(T initial) : state_(std::make_shared<State>()) {
    state_->value = std::move(initial);
  }
  Signal(const Signal&) = delete;
  Signal& operator=(const Signal&) = delete;

  const T& value() const { return state_->value; }

  /** Sets the value; returns whether it changed. */
  bool set(T v) {
    if (state_->value == v) return false;
    state_->value = std::move(v);
    notify();
    return true;
  }

  /** Sets the value and notifies even if equal (for identity-sensitive values). */
  void set_force(T v) {
    state_->value = std::move(v);
    notify();
  }

  [[nodiscard]] Subscription subscribe(Listener fn) const {
    return state_->changed.subscribe(std::move(fn));
  }

  size_t listener_count() const { return state_->changed.listener_count(); }
  void clear_listeners() const { state_->changed.clear(); }

 private:
  struct State {
    T value{};
    EventSource<T> changed;
    bool queued = false;
  };

  void notify() {
    auto& batch = detail::batch_state();
    if (batch.depth == 0) {
      auto keep = state_;
      keep->changed.emit(keep->value);
      return;
    }
    if (state_->queued) return;
    state_->queued = true;
    std::weak_ptr<State> weak = state_;
    batch.pending.push_back([weak] {
      if (auto s = weak.lock()) {
        s->queued = false;
        s->changed.emit(s->value);
      }
    });
  }

  std::shared_ptr<State> state_;
};

}  // namespace a2ui

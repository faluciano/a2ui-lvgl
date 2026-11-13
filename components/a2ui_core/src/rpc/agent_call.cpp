// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// AgentFunctionCall and AgentCallCache. See a2ui/agent_call.h.

#include "a2ui/agent_call.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace a2ui {
namespace {

void canonical_to(const Json& v, std::string& out) {
  switch (v.type()) {
    case Json::Type::Array: {
      out.push_back('[');
      bool first = true;
      for (const auto& e : v.as_array()) {
        if (!first) out.push_back(',');
        first = false;
        canonical_to(e.is_undefined() ? Json(nullptr) : e, out);
      }
      out.push_back(']');
      return;
    }
    case Json::Type::Object: {
      std::vector<const Json::Member*> members;
      for (const auto& m : v.as_object()) {
        if (!m.second.is_undefined()) members.push_back(&m);
      }
      std::sort(members.begin(), members.end(),
                [](const Json::Member* a, const Json::Member* b) { return a->first < b->first; });
      out.push_back('{');
      bool first = true;
      for (const auto* m : members) {
        if (!first) out.push_back(',');
        first = false;
        Json(m->first).dump_to(out);
        out.push_back(':');
        canonical_to(m->second, out);
      }
      out.push_back('}');
      return;
    }
    default: v.dump_to(out);
  }
}

}  // namespace

std::string canonical_json(const Json& value) {
  std::string out;
  canonical_to(value, out);
  return out;
}

std::string AgentFunctionCall::key() const {
  std::string out;
  Json(catalog_id).dump_to(out);
  out.push_back('|');
  Json(name).dump_to(out);
  out.push_back('|');
  canonical_to(args.is_object() ? args : Json::object(), out);
  return out;
}

Json AgentFunctionCall::to_call_function() const {
  return Json::object({{"@call", Json(name)},
                       {"catalogId", Json(catalog_id)},
                       {"args", args.is_object() ? args : Json::object()}});
}

// ---------------------------------------------------------------------------
// AgentCallCache

const AgentCallCache::Entry* AgentCallCache::find(const std::string& key) const {
  auto it = index_.find(key);
  return it == index_.end() ? nullptr : &it->second->second;
}

void AgentCallCache::put(const std::string& key, Entry entry) {
  forget(key);
  evict_if_full();
  order_.emplace_back(key, std::move(entry));
  index_[key] = std::prev(order_.end());
}

void AgentCallCache::mark_pending(const std::string& key) { put(key, Entry{State::Pending, Json()}); }

void AgentCallCache::answer(const std::string& key, Json value) {
  put(key, Entry{State::Answered, std::move(value)});
}

void AgentCallCache::fail(const std::string& key) { put(key, Entry{State::Failed, Json()}); }

void AgentCallCache::forget_failure(const std::string& key) {
  const Entry* e = find(key);
  if (e && e->state == State::Failed) forget(key);
}

void AgentCallCache::forget(const std::string& key) {
  auto it = index_.find(key);
  if (it == index_.end()) return;
  order_.erase(it->second);
  index_.erase(it);
}

void AgentCallCache::clear() {
  order_.clear();
  index_.clear();
}

bool AgentCallCache::has_pending() const {
  for (const auto& e : order_) {
    if (e.second.state == State::Pending) return true;
  }
  return false;
}

void AgentCallCache::evict_if_full() {
  size_t settled = 0;
  for (const auto& e : order_) settled += e.second.state != State::Pending;
  if (settled < max_settled_) return;
  for (auto it = order_.begin(); it != order_.end(); ++it) {
    if (it->second.state == State::Pending) continue;
    if (in_use_ && in_use_(it->first)) continue;
    index_.erase(it->first);
    order_.erase(it);
    return;
  }
}

}  // namespace a2ui

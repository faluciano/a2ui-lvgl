// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0

#include "a2ui/data_model.h"

namespace a2ui {

namespace path {

std::vector<std::string> split(std::string_view pointer) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i <= pointer.size()) {
    size_t j = pointer.find('/', i);
    if (j == std::string_view::npos) j = pointer.size();
    if (j > i) {
      std::string seg;
      seg.reserve(j - i);
      for (size_t k = i; k < j; ++k) {
        if (pointer[k] == '~' && k + 1 < j && (pointer[k + 1] == '0' || pointer[k + 1] == '1')) {
          seg.push_back(pointer[k + 1] == '1' ? '/' : '~');
          ++k;
        } else {
          seg.push_back(pointer[k]);
        }
      }
      out.push_back(std::move(seg));
    }
    i = j + 1;
  }
  return out;
}

std::string escape_segment(std::string_view segment) {
  std::string out;
  out.reserve(segment.size());
  for (char c : segment) {
    if (c == '~') {
      out += "~0";
    } else if (c == '/') {
      out += "~1";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string join(const std::vector<std::string>& segments) {
  if (segments.empty()) return "/";
  std::string out;
  for (const auto& s : segments) {
    out.push_back('/');
    out += escape_segment(s);
  }
  return out;
}

std::string normalize(std::string_view pointer) { return join(split(pointer)); }

std::string resolve(std::string_view p, std::string_view context) {
  if (is_absolute(p)) return normalize(p);
  if (p.empty() || p == ".") return normalize(context);
  std::string combined(context);
  combined.push_back('/');
  combined.append(p);
  return normalize(combined);
}

bool is_same_or_descendant(std::string_view p, std::string_view ancestor) {
  const auto a = split(ancestor);
  const auto s = split(p);
  if (a.size() > s.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] != s[i]) return false;
  }
  return true;
}

long parse_index(std::string_view segment) {
  if (segment.empty() || segment.size() > 10) return -1;
  if (segment.size() > 1 && segment[0] == '0') return -1;
  long v = 0;
  for (char c : segment) {
    if (c < '0' || c > '9') return -1;
    v = v * 10 + (c - '0');
  }
  return v;
}

}  // namespace path

namespace {

bool is_forbidden(const std::string& seg) {
  return seg == "__proto__" || seg == "constructor" || seg == "prototype";
}

Status check_forbidden(const std::vector<std::string>& segs, std::string_view pointer) {
  for (const auto& s : segs) {
    if (is_forbidden(s)) {
      return make_error(ErrorKind::Data, "Forbidden path segment '" + s + "' in path '" +
                                             std::string(pointer) + "'");
    }
  }
  return ok_status();
}

/** Looks up `segs[from..]` below `node`; undefined when absent. */
const Json* lookup(const Json* node, const std::vector<std::string>& segs, size_t from) {
  for (size_t i = from; i < segs.size() && node; ++i) {
    if (node->is_object()) {
      node = node->find(segs[i]);
    } else if (node->is_array()) {
      const long idx = path::parse_index(segs[i]);
      if (idx < 0 || static_cast<size_t>(idx) >= node->size()) return nullptr;
      node = &node->as_array()[static_cast<size_t>(idx)];
    } else {
      return nullptr;
    }
  }
  if (node && node->is_undefined()) return nullptr;
  return node;
}

Error data_error(std::string_view pointer, const std::string& why) {
  return make_error(ErrorKind::Data, "Cannot set path '" + std::string(pointer) + "': " + why);
}

}  // namespace

DataModel::DataModel(Json initial)
    : root_(initial.is_nullish() ? Json::object() : std::move(initial)),
      observers_(make_ref<std::vector<Ref<Observer>>>()) {}

Result<const Json*> DataModel::get(std::string_view pointer) const {
  const auto segs = path::split(pointer);
  A2UI_RETURN_IF_ERROR(check_forbidden(segs, pointer));
  return lookup(&root_, segs, 0);
}

Json DataModel::value_at(std::string_view pointer) const {
  auto r = get(pointer);
  if (!r.ok() || r.value() == nullptr) return Json();
  return *r.value();
}

Status DataModel::set(std::string_view pointer, Json value) {
  const auto segs = path::split(pointer);
  A2UI_RETURN_IF_ERROR(check_forbidden(segs, pointer));
  const bool removing = value.is_nullish();

  if (segs.empty()) {
    Json next = removing ? Json::object() : std::move(value);
    if (next == root_) return ok_status();
    Json old = std::move(root_);
    root_ = std::move(next);
    notify(segs, old, true);
    return ok_status();
  }

  const Json* existing = lookup(&root_, segs, 0);
  if (removing && existing == nullptr) return ok_status();  // Rule 7: nothing to remove.
  if (!removing && existing && *existing == value) return ok_status();
  Json old = existing ? *existing : Json();

  // Dry run: validate every segment before mutating, so a failed write leaves
  // no half-built containers behind.
  {
    const Json* cur = &root_;
    Json::Type virtual_type = Json::Type::Undefined;  // Type of a not-yet-created container.
    for (size_t i = 0; i < segs.size(); ++i) {
      const bool last = i + 1 == segs.size();
      const Json::Type t = cur ? (cur->is_nullish() ? Json::Type::Undefined : cur->type())
                               : virtual_type;
      if (t == Json::Type::Array) {
        const long idx = path::parse_index(segs[i]);
        if (idx < 0) {
          return data_error(pointer, "non-numeric segment '" + segs[i] + "' for a list");
        }
        if (idx > kMaxArrayIndex) {
          return data_error(pointer, "list index " + segs[i] + " exceeds the maximum of " +
                                         std::to_string(kMaxArrayIndex));
        }
        cur = (cur && static_cast<size_t>(idx) < cur->size())
                  ? &cur->as_array()[static_cast<size_t>(idx)]
                  : nullptr;
      } else if (t == Json::Type::Object) {
        cur = cur ? cur->find(segs[i]) : nullptr;
      } else if (t == Json::Type::Undefined) {
        // Only reachable for a container about to be created (or a null placeholder).
        cur = nullptr;
      } else {
        return data_error(pointer, "segment '" + (i == 0 ? std::string("/") : segs[i - 1]) +
                                       "' holds a primitive value");
      }
      if (!last && (cur == nullptr || cur->is_nullish())) {
        virtual_type = path::parse_index(segs[i + 1]) >= 0 ? Json::Type::Array : Json::Type::Object;
        cur = nullptr;
      }
    }
  }

  // Apply.
  Json* cur = &root_;
  for (size_t i = 0; i + 1 < segs.size(); ++i) {
    Json* child;
    if (cur->is_array()) {
      auto& arr = cur->as_array_mut();
      const size_t idx = static_cast<size_t>(path::parse_index(segs[i]));
      if (idx >= arr.size()) arr.resize(idx + 1);
      child = &arr[idx];
    } else {
      child = cur->find_mut(segs[i]);
      if (!child) {
        cur->set(segs[i], Json());
        child = cur->find_mut(segs[i]);
      }
    }
    if (child->is_nullish()) {
      *child = path::parse_index(segs[i + 1]) >= 0 ? Json(Json::Array{}) : Json::object();
    }
    cur = child;
  }
  const std::string& leaf = segs.back();
  if (cur->is_array()) {
    auto& arr = cur->as_array_mut();
    const size_t idx = static_cast<size_t>(path::parse_index(leaf));
    if (removing) {
      if (idx < arr.size()) arr[idx] = Json();  // Rule 6: keep length, leave a hole.
    } else {
      if (idx >= arr.size()) arr.resize(idx + 1);
      arr[idx] = std::move(value);
    }
  } else if (removing) {
    cur->erase(leaf);
  } else {
    cur->set(leaf, std::move(value));
  }
  notify(segs, old, true);
  return ok_status();
}

void DataModel::notify(const std::vector<std::string>& written, const Json& old_value, bool) {
  auto observers = *observers_;  // Snapshot: listeners may (un)subscribe.
  for (const auto& obs : observers) {
    if (!obs->alive) continue;
    const auto& os = obs->segments;
    bool related = true;
    const size_t common = os.size() < written.size() ? os.size() : written.size();
    for (size_t i = 0; i < common; ++i) {
      if (os[i] != written[i]) {
        related = false;
        break;
      }
    }
    if (!related) continue;
    const Json* now = lookup(&root_, os, 0);
    if (os.size() > written.size()) {
      // Descendant of the write: only notify if its own value changed.
      const Json* before = lookup(&old_value, os, written.size());
      const bool same = (before == nullptr && now == nullptr) ||
                        (before != nullptr && now != nullptr && *before == *now);
      if (same) continue;
    }
    obs->listener(now ? *now : Json());
  }
  on_changed_.emit(path::join(written));
}

Subscription DataModel::subscribe(std::string_view pointer, Listener listener) {
  auto obs = make_ref<Observer>();
  obs->segments = path::split(pointer);
  obs->path = path::join(obs->segments);
  obs->listener = std::move(listener);
  observers_->push_back(obs);
  WeakRef<std::vector<Ref<Observer>>> weak_list = observers_;
  WeakRef<Observer> weak_obs = obs;
  return Subscription([weak_list, weak_obs] {
    auto list = weak_list.lock();
    auto o = weak_obs.lock();
    if (!list || !o) return;
    o->alive = false;
    for (auto it = list->begin(); it != list->end(); ++it) {
      if (*it == o) {
        list->erase(it);
        break;
      }
    }
  });
}

size_t DataModel::observer_count() const { return observers_->size(); }

void DataModel::dispose() {
  for (auto& o : *observers_) o->alive = false;
  observers_->clear();
  on_changed_.clear();
}

}  // namespace a2ui

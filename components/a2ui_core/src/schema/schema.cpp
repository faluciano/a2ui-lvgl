// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// JSON Schema (draft 2020-12 subset) interpreter. See a2ui/schema.h.

#include "a2ui/schema.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "a2ui/identifier.h"

namespace a2ui {

// ---------------------------------------------------------------------------
// Pointers and references

std::string escape_pointer_token(std::string_view token) {
  std::string out;
  out.reserve(token.size());
  for (char c : token) {
    if (c == '~') {
      out += "~0";
    } else if (c == '/') {
      out += "~1";
    } else {
      out += c;
    }
  }
  return out;
}

namespace {

// --- format: date, time, date-time ------------------------------------------------
//
// The basic catalog tells date, time and date-time strings apart with a
// oneOf of `format`s (DateTimeInput min/max), which works only when those
// formats assert. They are checked as the ISO 8601 shapes the catalog uses
// ("yyyy-MM-dd", "HH:mm[:ss[.fff]]" with an optional zone, and the two
// joined by 'T'); every other format stays an annotation.

bool digits_at(std::string_view s, size_t pos, size_t n, int* out) {
  if (pos + n > s.size()) return false;
  int v = 0;
  for (size_t i = 0; i < n; ++i) {
    const char c = s[pos + i];
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  *out = v;
  return true;
}

bool is_iso_date(std::string_view s) {
  int y = 0, m = 0, d = 0;
  if (s.size() != 10 || s[4] != '-' || s[7] != '-' || !digits_at(s, 0, 4, &y) || !digits_at(s, 5, 2, &m) ||
      !digits_at(s, 8, 2, &d) || m < 1 || m > 12 || d < 1) {
    return false;
  }
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  return d <= (m == 2 && leap ? 29 : kDays[m - 1]);
}

/** HH:mm[:ss[.f+]] followed by an optional Z, ±HH, ±HHmm or ±HH:mm. */
bool is_iso_time(std::string_view s) {
  int h = 0, mi = 0, sec = 0;
  if (s.size() < 5 || s[2] != ':' || !digits_at(s, 0, 2, &h) || !digits_at(s, 3, 2, &mi) || h > 23 || mi > 59) {
    return false;
  }
  size_t pos = 5;
  if (pos < s.size() && s[pos] == ':') {
    if (!digits_at(s, pos + 1, 2, &sec) || sec > 60) return false;  // 60: leap second.
    pos += 3;
    if (pos < s.size() && s[pos] == '.') {
      const size_t start = ++pos;
      while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
      if (pos == start) return false;
    }
  }
  if (pos == s.size()) return true;
  if (s[pos] == 'Z' || s[pos] == 'z') return pos + 1 == s.size();
  if (s[pos] != '+' && s[pos] != '-') return false;
  const std::string_view zone = s.substr(pos + 1);
  int zh = 0, zm = 0;
  if (!digits_at(zone, 0, 2, &zh) || zh > 23) return false;
  if (zone.size() == 2) return true;
  if (zone.size() == 4) return digits_at(zone, 2, 2, &zm) && zm <= 59;
  return zone.size() == 5 && zone[2] == ':' && digits_at(zone, 3, 2, &zm) && zm <= 59;
}

bool is_iso_date_time(std::string_view s) {
  return s.size() > 11 && (s[10] == 'T' || s[10] == 't') && is_iso_date(s.substr(0, 10)) &&
         is_iso_time(s.substr(11));
}

/** False when `str` fails an asserted format; true for formats kept as annotations. */
bool format_holds(std::string_view format, std::string_view str) {
  if (format == "date") return is_iso_date(str);
  if (format == "time") return is_iso_time(str);
  if (format == "date-time") return is_iso_date_time(str);
  return true;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string unescape_token(std::string_view raw) {
  std::string pct;
  pct.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '%' && i + 2 < raw.size()) {
      const int hi = hex_value(raw[i + 1]);
      const int lo = hex_value(raw[i + 2]);
      if (hi >= 0 && lo >= 0) {
        pct.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
        continue;
      }
    }
    pct.push_back(raw[i]);
  }
  std::string out;
  out.reserve(pct.size());
  for (size_t i = 0; i < pct.size(); ++i) {
    if (pct[i] == '~' && i + 1 < pct.size() && (pct[i + 1] == '0' || pct[i + 1] == '1')) {
      out.push_back(pct[i + 1] == '0' ? '~' : '/');
      ++i;
    } else {
      out.push_back(pct[i]);
    }
  }
  return out;
}

std::string_view basename_of(std::string_view s) {
  const size_t slash = s.rfind('/');
  return slash == std::string_view::npos ? s : s.substr(slash + 1);
}

bool is_meta_schema(std::string_view doc_part) {
  return doc_part.rfind("https://json-schema.org/", 0) == 0 ||
         doc_part.rfind("http://json-schema.org/", 0) == 0;
}

}  // namespace

void split_ref(std::string_view ref, std::string_view* doc_part, std::string_view* pointer) {
  const size_t hash = ref.find('#');
  if (hash == std::string_view::npos) {
    *doc_part = ref;
    *pointer = std::string_view();
  } else {
    *doc_part = ref.substr(0, hash);
    *pointer = ref.substr(hash + 1);
  }
}

const Json* resolve_json_pointer(const Json& root, std::string_view pointer) {
  const Json* cur = &root;
  if (pointer.empty()) return cur;
  if (pointer[0] != '/') return nullptr;  // Plain-name anchors are not supported.
  size_t pos = 1;
  for (;;) {
    size_t end = pointer.find('/', pos);
    if (end == std::string_view::npos) end = pointer.size();
    const std::string token = unescape_token(pointer.substr(pos, end - pos));
    if (cur->is_object()) {
      cur = cur->find(token);
      if (!cur) return nullptr;
    } else if (cur->is_array()) {
      if (token.empty() || token.size() > 9) return nullptr;
      size_t idx = 0;
      for (char c : token) {
        if (c < '0' || c > '9') return nullptr;
        idx = idx * 10 + static_cast<size_t>(c - '0');
      }
      if (token.size() > 1 && token[0] == '0') return nullptr;
      if (idx >= cur->size()) return nullptr;
      cur = &(*cur)[idx];
    } else {
      return nullptr;
    }
    if (end == pointer.size()) return cur;
    pos = end + 1;
  }
}

SchemaNode resolve_node_pointer(SchemaNode root, std::string_view pointer) {
  if (const Json* j = root.json()) return SchemaNode(resolve_json_pointer(*j, pointer));
  SchemaNode cur = root;
  if (pointer.empty() || !cur) return cur;
  if (pointer[0] != '/') return {};
  size_t pos = 1;
  for (;;) {
    size_t end = pointer.find('/', pos);
    if (end == std::string_view::npos) end = pointer.size();
    const std::string token = unescape_token(pointer.substr(pos, end - pos));
    if (cur.is_object()) {
      cur = cur.find(token);
      if (!cur) return {};
    } else if (cur.is_array()) {
      if (token.empty() || token.size() > 9) return {};
      size_t idx = 0;
      for (char c : token) {
        if (c < '0' || c > '9') return {};
        idx = idx * 10 + static_cast<size_t>(c - '0');
      }
      if (token.size() > 1 && token[0] == '0') return {};
      if (idx >= cur.size()) return {};
      cur = cur.at(idx);
    } else {
      return {};
    }
    if (end == pointer.size()) return cur;
    pos = end + 1;
  }
}

Json issues_to_json(const std::vector<ValidationIssue>& issues) {
  Json out = Json::array();
  for (const auto& i : issues) {
    out.push_back(Json::object({{"path", Json(i.instance_path)},
                                {"keyword", Json(i.keyword)},
                                {"message", Json(i.message)}}));
  }
  return out;
}

// ---------------------------------------------------------------------------
// Registry

void SchemaRegistry::add(std::string name, Ref<const Json> document) {
  add(std::move(name), std::move(document), nullptr);
}

void SchemaRegistry::add(std::string name, Ref<const Json> document,
                         PointerResolver resolver) {
  Doc d;
  d.name = std::move(name);
  if (document) {
    const Json& id = (*document)["$id"];
    if (id.is_string()) d.id = id.as_string();
  }
  d.json = std::move(document);
  d.resolver = std::move(resolver);
  for (auto& existing : docs_) {
    if (existing.name == d.name) {
      existing = std::move(d);
      return;
    }
  }
  docs_.push_back(std::move(d));
}

void SchemaRegistry::add(std::string name, const PackedDocument& document) {
  Doc d;
  d.name = std::move(name);
  const SchemaNode id = SchemaNode::root(document)["$id"];
  if (id.is_string()) d.id = std::string(id.as_string());
  d.packed = &document;
  for (auto& existing : docs_) {
    if (existing.name == d.name) {
      existing = std::move(d);
      return;
    }
  }
  docs_.push_back(std::move(d));
}

void SchemaRegistry::add_override(std::string ref, Ref<const Json> schema) {
  for (auto& o : overrides_) {
    if (o.key == ref) {
      o.schema = std::move(schema);
      return;
    }
  }
  overrides_.push_back({std::move(ref), std::move(schema)});
}

int SchemaRegistry::find_document(std::string_view doc_part) const {
  for (size_t i = 0; i < docs_.size(); ++i) {
    if (docs_[i].name == doc_part || (!docs_[i].id.empty() && docs_[i].id == doc_part)) {
      return static_cast<int>(i);
    }
  }
  const std::string_view base = basename_of(doc_part);
  if (base.empty()) return -1;
  for (size_t i = 0; i < docs_.size(); ++i) {
    if (basename_of(docs_[i].name) == base ||
        (!docs_[i].id.empty() && basename_of(docs_[i].id) == base)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

SchemaNode SchemaRegistry::resolve_pointer(int doc, std::string_view pointer) const {
  if (doc < 0 || static_cast<size_t>(doc) >= docs_.size()) return {};
  const Doc& d = docs_[static_cast<size_t>(doc)];
  if (d.resolver) return d.resolver(pointer);
  if (d.packed) return resolve_node_pointer(SchemaNode::root(*d.packed), pointer);
  return d.json ? SchemaNode(resolve_json_pointer(*d.json, pointer)) : SchemaNode();
}

const Json* SchemaRegistry::override_for(int doc, std::string_view pointer) const {
  if (overrides_.empty() || doc < 0 || static_cast<size_t>(doc) >= docs_.size()) return nullptr;
  const std::string& name = docs_[static_cast<size_t>(doc)].name;
  for (const auto& o : overrides_) {
    if (o.key.size() == name.size() + 1 + pointer.size() &&
        o.key.compare(0, name.size(), name) == 0 && o.key[name.size()] == '#' &&
        o.key.compare(name.size() + 1, std::string::npos, pointer.data(), pointer.size()) == 0) {
      return o.schema.get();
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Validator
//
// Schemas are SchemaNode views, so one schema can mix Json (wrappers built
// at run time, overrides) and packed documents reached through $ref.

namespace {

/** Annotations for unevaluatedProperties / unevaluatedItems. */
struct Annot {
  std::vector<const std::string*> props;  // Instance keys (by address) evaluated.
  size_t items = 0;                       // Leading items evaluated.
  bool all_items = false;
  std::vector<size_t> item_idx;           // Items evaluated by `contains`.

  void merge(const Annot& o) {
    props.insert(props.end(), o.props.begin(), o.props.end());
    items = std::max(items, o.items);
    all_items = all_items || o.all_items;
    item_idx.insert(item_idx.end(), o.item_idx.begin(), o.item_idx.end());
  }
  bool has_prop(const std::string* k) const {
    return std::find(props.begin(), props.end(), k) != props.end();
  }
};

const Json& true_schema() {
  static const Json t(true);
  return t;
}

const char* type_name(const Json& v) {
  switch (v.type()) {
    case Json::Type::Undefined: return "undefined";
    case Json::Type::Null: return "null";
    case Json::Type::Bool: return "boolean";
    case Json::Type::Number: return "number";
    case Json::Type::String: return "string";
    case Json::Type::Array: return "array";
    case Json::Type::Object: return "object";
  }
  return "?";
}

bool type_matches(std::string_view t, const Json& v) {
  if (t == "object") return v.is_object();
  if (t == "array") return v.is_array();
  if (t == "string") return v.is_string();
  if (t == "number") return v.is_number();
  if (t == "integer") {
    if (!v.is_number()) return false;
    const double d = v.as_number();
    return std::isfinite(d) && std::floor(d) == d;
  }
  if (t == "boolean") return v.is_bool();
  if (t == "null") return v.is_null();
  return false;
}

std::string shorten(std::string s) {
  if (s.size() > 60) {
    s.resize(57);
    s += "...";
  }
  return s;
}
std::string short_dump(const Json& v) { return shorten(v.dump()); }
std::string short_dump(SchemaNode v) { return shorten(v.dump()); }

class Validator {
 public:
  Validator(const SchemaRegistry& reg, const SchemaValidateOptions& opt)
      : reg_(reg), opt_(opt) {}

  std::vector<ValidationIssue> issues;
  bool recursion_hit = false;
  bool unresolved_ref = false;
  std::string path;

  bool eval(int doc, SchemaNode schema, const Json& inst, Annot* annot, bool report, int depth) {
    // Null relaxation (SchemaValidateOptions::accept_null_properties): the
    // caller sets next_ for this call only; scope_ is this call's scope while
    // it runs and is restored on return.
    const NullScope mine = next_;
    next_ = NullScope::None;
    const ScopeGuard guard(&scope_, mine);
    if (mine == NullScope::Value && inst.is_null()) return true;
    if (schema.is_bool()) {
      if (schema.as_bool()) return true;
      if (report) add_issue("false", "no value is allowed here");
      return false;
    }
    if (!schema.is_object()) return true;
    if (depth > opt_.max_depth) {
      recursion_hit = true;
      if (report) add_issue("$ref", "schema nesting exceeds the depth limit");
      return false;
    }

    bool valid = true;
    Annot local;
#define A2UI_FAIL()          \
  do {                       \
    valid = false;           \
    if (!report) return false; \
  } while (0)

    // --- $ref
    if (SchemaNode ref = schema.find("$ref"); ref.is_string()) {
      int tdoc = doc;
      const SchemaNode target = resolve(doc, ref.as_string(), &tdoc);
      if (!target) {
        unresolved_ref = true;
        if (report) add_issue("$ref", "unresolvable reference '" + std::string(ref.as_string()) + "'");
        A2UI_FAIL();
      } else {
        Annot sub;
        if (eval(tdoc, target, inst, &sub, report, depth + 1)) {
          local.merge(sub);
        } else {
          A2UI_FAIL();
        }
      }
    }

    // --- type / enum / const
    if (SchemaNode t = schema.find("type")) {
      bool ok = false;
      if (t.is_string()) {
        ok = type_matches(t.as_string(), inst);
      } else if (t.is_array()) {
        for (SchemaNode e : t.items()) {
          if (e.is_string() && type_matches(e.as_string(), inst)) {
            ok = true;
            break;
          }
        }
      } else {
        ok = true;
      }
      if (!ok) {
        if (report) add_issue("type", "expected " + type_list(t) + ", got " + type_name(inst));
        A2UI_FAIL();
      }
    }
    if (SchemaNode e = schema.find("enum"); e.is_array()) {
      bool ok = false;
      for (SchemaNode v : e.items()) {
        if (v.equals(inst)) {
          ok = true;
          break;
        }
      }
      if (!ok) {
        if (report) add_issue("enum", short_dump(inst) + " is not one of " + short_dump(e));
        A2UI_FAIL();
      }
    }
    if (SchemaNode c = schema.find("const")) {
      if (!c.equals(inst)) {
        if (report) add_issue("const", "expected " + short_dump(c) + ", got " + short_dump(inst));
        A2UI_FAIL();
      }
    }

    // --- numbers
    if (inst.is_number()) {
      const double x = inst.as_number();
      if (!check_number(schema, x, report)) A2UI_FAIL();
    }

    // --- strings
    if (inst.is_string()) {
      if (!check_string(schema, inst.as_string(), report)) A2UI_FAIL();
    }

    // --- in-place applicators
    if (SchemaNode all = schema.find("allOf"); all.is_array()) {
      for (SchemaNode sub : all.items()) {
        Annot a;
        next_ = combiner_scope();
        if (eval(doc, sub, inst, &a, report, depth + 1)) {
          local.merge(a);
        } else {
          A2UI_FAIL();
        }
      }
    }
    if (SchemaNode any = schema.find("anyOf"); any.is_array()) {
      bool ok = false;
      for (SchemaNode sub : any.items()) {
        Annot a;
        next_ = combiner_scope();
        if (eval(doc, sub, inst, &a, false, depth + 1)) {
          ok = true;
          local.merge(a);  // Every passing branch contributes annotations.
        }
      }
      if (!ok) {
        if (report) {
          add_issue("anyOf", "value does not match any allowed schema");
          explain_branches(doc, any, inst, depth);
        }
        A2UI_FAIL();
      }
    }
    if (SchemaNode one = schema.find("oneOf"); one.is_array()) {
      size_t matched = 0;
      Annot first;
      for (SchemaNode sub : one.items()) {
        Annot a;
        next_ = combiner_scope();
        if (eval(doc, sub, inst, &a, false, depth + 1)) {
          if (++matched == 1) first = std::move(a);
          if (matched > 1) break;
        }
      }
      if (matched == 1) {
        local.merge(first);
      } else {
        if (report) {
          if (matched == 0) {
            add_issue("oneOf", "value does not match any allowed schema");
            explain_branches(doc, one, inst, depth);
          } else {
            add_issue("oneOf", "value matches more than one schema");
          }
        }
        A2UI_FAIL();
      }
    }
    if (SchemaNode n = schema.find("not")) {
      Annot ignored;
      if (eval(doc, n, inst, &ignored, false, depth + 1)) {
        if (report) add_issue("not", "value matches a schema it must not match");
        A2UI_FAIL();
      }
    }
    if (SchemaNode cond = schema.find("if")) {
      Annot a;
      const bool if_ok = eval(doc, cond, inst, &a, false, depth + 1);
      const SchemaNode branch = schema.find(if_ok ? "then" : "else");
      if (if_ok) local.merge(a);
      if (branch) {
        Annot b;
        if (eval(doc, branch, inst, &b, report, depth + 1)) {
          local.merge(b);
        } else {
          A2UI_FAIL();
        }
      }
    }

    // --- objects
    if (inst.is_object()) {
      if (!check_object(doc, schema, inst, &local, report, depth)) A2UI_FAIL();
    }

    // --- arrays
    if (inst.is_array()) {
      if (!check_array(doc, schema, inst, &local, report, depth)) A2UI_FAIL();
    }

    // --- unevaluated*
    // Skipped once the schema has failed: annotations of failed subschemas
    // are dropped, so every property would look unevaluated and bury the
    // real issue under noise. The result is invalid either way.
    if (valid && inst.is_object()) {
      if (SchemaNode u = schema.find("unevaluatedProperties")) {
        for (const auto& m : inst.as_object()) {
          if (local.has_prop(&m.first)) continue;
          if (!descend_key(doc, u, m.first, m.second, report, depth, "unevaluatedProperties")) {
            A2UI_FAIL();
          }
          local.props.push_back(&m.first);
        }
      }
    }
    if (valid && inst.is_array()) {
      if (SchemaNode u = schema.find("unevaluatedItems")) {
        if (!local.all_items) {
          for (size_t i = local.items; i < inst.size(); ++i) {
            if (std::find(local.item_idx.begin(), local.item_idx.end(), i) != local.item_idx.end()) {
              continue;
            }
            if (!descend_index(doc, u, i, inst[i], report, depth, "unevaluatedItems")) A2UI_FAIL();
          }
        }
        local.all_items = true;
      }
    }
#undef A2UI_FAIL

    if (valid && annot) annot->merge(local);
    return valid;
  }

  SchemaNode resolve(int doc, std::string_view ref, int* out_doc) {
    std::string_view doc_part, pointer;
    split_ref(ref, &doc_part, &pointer);
    if (!doc_part.empty() && is_meta_schema(doc_part)) {
      *out_doc = doc;
      return true_schema();
    }
    const int target = doc_part.empty() ? doc : reg_.find_document(doc_part);
    if (target < 0) return {};
    *out_doc = target;
    if (const Json* o = reg_.override_for(target, pointer)) return o;
    return reg_.resolve_pointer(target, pointer);
  }

  void add_issue(const char* keyword, std::string message) {
    if (issues.size() >= opt_.max_issues) return;
    issues.push_back({path, keyword, std::move(message)});
  }

 private:
  static std::string type_list(SchemaNode t) {
    if (t.is_string()) return std::string(t.as_string());
    std::string out;
    for (SchemaNode e : t.items()) {
      if (!out.empty()) out += " or ";
      out += e.as_string();
    }
    return out;
  }

  // When no branch of anyOf/oneOf matched, reports the issues of the single
  // branch that could apply, if exactly one does (by `type` or by `const`
  // members such as "@call"), to give a specific message.
  void explain_branches(int doc, SchemaNode branches, const Json& inst, int depth) {
    SchemaNode candidate;
    int candidates = 0;
    for (SchemaNode b : branches.items()) {
      if (!plausible(doc, b, inst, 0)) continue;
      candidate = b;
      ++candidates;
    }
    if (candidates == 1) {
      Annot ignored;
      next_ = combiner_scope();
      eval(doc, candidate, inst, &ignored, true, depth + 1);
    }
  }

  // A cheap guess whether `inst` was meant for branch `b`: its type agrees
  // and every `const`-valued property the branch requires is present.
  bool plausible(int doc, SchemaNode b, const Json& inst, int hops) {
    if (!b.is_object() || hops > 8) return false;
    if (SchemaNode ref = b.find("$ref"); ref.is_string()) {
      int tdoc = doc;
      const SchemaNode t = resolve(doc, ref.as_string(), &tdoc);
      if (t && !plausible(tdoc, t, inst, hops + 1)) return false;
      if (!t) return false;
    }
    if (SchemaNode t = b.find("type"); t.is_string() && !type_matches(t.as_string(), inst)) {
      return false;
    }
    if (SchemaNode props = b.find("properties"); props.is_object() && inst.is_object()) {
      for (const auto& m : props.members()) {
        if (SchemaNode c = m.value.find("const")) {
          const Json* v = inst.find(m.key);
          if (v && !c.equals(*v)) return false;
        }
      }
      if (SchemaNode req = b.find("required"); req.is_array()) {
        for (SchemaNode r : req.items()) {
          if (r.is_string() && !inst.contains(r.as_string())) return false;
        }
      }
    }
    if (SchemaNode all = b.find("allOf"); all.is_array()) {
      for (SchemaNode s : all.items()) {
        if (s.is_object() && !plausible(doc, s, inst, hops + 1)) return false;
      }
    }
    return true;
  }

  bool check_number(SchemaNode s, double x, bool report) {
    bool ok = true;
    auto num = [&](const char* k) -> SchemaNode {
      const SchemaNode v = s.find(k);
      return v.is_number() ? v : SchemaNode();
    };
    if (SchemaNode v = num("minimum"); v && x < v.as_number()) {
      if (report) add_issue("minimum", number_to_string(x) + " is less than " + v.dump());
      ok = false;
    }
    if (SchemaNode v = num("maximum"); v && x > v.as_number()) {
      if (report) add_issue("maximum", number_to_string(x) + " is greater than " + v.dump());
      ok = false;
    }
    if (SchemaNode v = num("exclusiveMinimum"); v && x <= v.as_number()) {
      if (report) add_issue("exclusiveMinimum", number_to_string(x) + " must be greater than " + v.dump());
      ok = false;
    }
    if (SchemaNode v = num("exclusiveMaximum"); v && x >= v.as_number()) {
      if (report) add_issue("exclusiveMaximum", number_to_string(x) + " must be less than " + v.dump());
      ok = false;
    }
    if (SchemaNode v = num("multipleOf"); v && v.as_number() > 0) {
      const double q = x / v.as_number();
      if (!std::isfinite(q) || std::fabs(q - std::round(q)) > 1e-9) {
        if (report) add_issue("multipleOf", number_to_string(x) + " is not a multiple of " + v.dump());
        ok = false;
      }
    }
    return ok;
  }

  bool check_string(SchemaNode s, const std::string& str, bool report) {
    bool ok = true;
    const SchemaNode min_len = s.find("minLength");
    const SchemaNode max_len = s.find("maxLength");
    if (min_len.is_number() || max_len.is_number()) {
      const double len = static_cast<double>(utf8_length(str));
      if (min_len.is_number() && len < min_len.as_number()) {
        if (report) add_issue("minLength", "string is shorter than " + min_len.dump());
        ok = false;
      }
      if (max_len.is_number() && len > max_len.as_number()) {
        if (report) add_issue("maxLength", "string is longer than " + max_len.dump());
        ok = false;
      }
    }
    if (SchemaNode f = s.find("format"); f.is_string() && !format_holds(f.as_string(), str)) {
      if (report) add_issue("format", short_dump(Json(str)) + " is not a valid " + std::string(f.as_string()));
      ok = false;
    }
    if (SchemaNode p = s.find("pattern"); p.is_string()) {
      bool parsed = true;
      if (!pattern_search(p.as_string(), str, &parsed)) {
        if (report) {
          const std::string pattern(p.as_string());
          add_issue("pattern", parsed ? short_dump(Json(str)) + " does not match '" + pattern + "'"
                                      : "unsupported pattern '" + pattern + "'");
        }
        ok = false;
      }
    }
    return ok;
  }

  bool pattern_search(std::string_view pattern, const std::string& text, bool* parsed) {
    // UAX #31 identifier patterns are matched directly: they are the most
    // common pattern in the A2UI schemas (Extensions keys).
    if (pattern == "^[\\p{XID_Start}_][\\p{XID_Continue}]*$") {
      *parsed = true;
      return is_uax31_identifier(text);
    }
    return schema_pattern::search(pattern, text, parsed);
  }

  bool descend_key(int doc, SchemaNode sub, const std::string& key, const Json& value,
                   bool report, int depth, const char* keyword) {
    const size_t mark = path.size();
    path += '/';
    path += escape_pointer_token(key);
    const size_t before = issues.size();
    if (std::string_view(keyword) == "properties" && scope_ != NullScope::None) next_ = NullScope::Value;
    const bool ok = eval(doc, sub, value, nullptr, report, depth + 1);
    if (!ok && report && issues.size() == before) {
      add_issue(keyword, sub.is_bool() ? "property '" + key + "' is not allowed"
                                       : "invalid property '" + key + "'");
    }
    if (!ok && report && sub.is_bool() && issues.size() > before &&
        issues.back().keyword == std::string("false")) {
      issues.back().keyword = keyword;
      issues.back().message = "property '" + key + "' is not allowed";
    }
    path.resize(mark);
    return ok;
  }

  bool descend_index(int doc, SchemaNode sub, size_t index, const Json& value, bool report,
                     int depth, const char* keyword) {
    const size_t mark = path.size();
    path += '/';
    path += std::to_string(index);
    if (std::string_view(keyword) == "items" && scope_ == NullScope::Value) next_ = NullScope::Value;
    const bool ok = eval(doc, sub, value, nullptr, report, depth + 1);
    if (!ok && report && sub.is_bool() && !issues.empty() && issues.back().keyword == std::string("false")) {
      issues.back().keyword = keyword;
      issues.back().message = "item " + std::to_string(index) + " is not allowed";
    }
    path.resize(mark);
    return ok;
  }

  bool check_object(int doc, SchemaNode s, const Json& inst, Annot* annot, bool report, int depth) {
    bool ok = true;
#define A2UI_OFAIL()           \
  do {                         \
    ok = false;                \
    if (!report) return false; \
  } while (0)
    if (SchemaNode req = s.find("required"); req.is_array()) {
      for (SchemaNode r : req.items()) {
        if (r.is_string() && !inst.contains(r.as_string())) {
          if (report) add_issue("required", "missing required property '" + std::string(r.as_string()) + "'");
          A2UI_OFAIL();
        }
      }
    }
    if (SchemaNode v = s.find("minProperties"); v.is_number() &&
        static_cast<double>(inst.size()) < v.as_number()) {
      if (report) add_issue("minProperties", "object has fewer than " + v.dump() + " properties");
      A2UI_OFAIL();
    }
    if (SchemaNode v = s.find("maxProperties"); v.is_number() &&
        static_cast<double>(inst.size()) > v.as_number()) {
      if (report) add_issue("maxProperties", "object has more than " + v.dump() + " properties");
      A2UI_OFAIL();
    }
    if (SchemaNode dr = s.find("dependentRequired"); dr.is_object()) {
      for (const auto& m : dr.members()) {
        if (!inst.contains(m.key) || !m.value.is_array()) continue;
        for (SchemaNode r : m.value.items()) {
          if (r.is_string() && !inst.contains(r.as_string())) {
            if (report) {
              add_issue("dependentRequired", "property '" + std::string(r.as_string()) + "' is required with '" +
                                                 std::string(m.key) + "'");
            }
            A2UI_OFAIL();
          }
        }
      }
    }
    if (SchemaNode ds = s.find("dependentSchemas"); ds.is_object()) {
      for (const auto& m : ds.members()) {
        if (!inst.contains(m.key)) continue;
        Annot a;
        if (eval(doc, m.value, inst, &a, report, depth + 1)) {
          annot->merge(a);
        } else {
          A2UI_OFAIL();
        }
      }
    }
    if (SchemaNode pn = s.find("propertyNames")) {
      for (const auto& m : inst.as_object()) {
        const Json key(m.first);
        const size_t before = issues.size();
        if (!eval(doc, pn, key, nullptr, false, depth + 1)) {
          if (report && issues.size() == before) {
            const size_t mark = path.size();
            path += '/';
            path += escape_pointer_token(m.first);
            add_issue("propertyNames", "property name '" + m.first + "' is not allowed");
            path.resize(mark);
          }
          A2UI_OFAIL();
        }
      }
    }

    const SchemaNode props = s.find("properties");
    const SchemaNode pattern_props = s.find("patternProperties");
    const SchemaNode additional = s.find("additionalProperties");
    if (props || pattern_props || additional) {
      for (const auto& m : inst.as_object()) {
        bool matched = false;
        if (props.is_object()) {
          if (SchemaNode sub = props.find(m.first)) {
            matched = true;
            if (!descend_key(doc, sub, m.first, m.second, report, depth, "properties")) A2UI_OFAIL();
          }
        }
        if (pattern_props.is_object()) {
          for (const auto& pp : pattern_props.members()) {
            bool parsed = true;
            if (!pattern_search(pp.key, m.first, &parsed)) continue;
            matched = true;
            if (!descend_key(doc, pp.value, m.first, m.second, report, depth, "patternProperties")) {
              A2UI_OFAIL();
            }
          }
        }
        if (!matched && additional) {
          matched = true;
          if (!descend_key(doc, additional, m.first, m.second, report, depth, "additionalProperties")) {
            A2UI_OFAIL();
          }
        }
        if (matched) annot->props.push_back(&m.first);
      }
    }
#undef A2UI_OFAIL
    return ok;
  }

  bool check_array(int doc, SchemaNode s, const Json& inst, Annot* annot, bool report, int depth) {
    bool ok = true;
#define A2UI_AFAIL()           \
  do {                         \
    ok = false;                \
    if (!report) return false; \
  } while (0)
    const size_t n = inst.size();
    if (SchemaNode v = s.find("minItems"); v.is_number() && static_cast<double>(n) < v.as_number()) {
      if (report) add_issue("minItems", "array has fewer than " + v.dump() + " items");
      A2UI_AFAIL();
    }
    if (SchemaNode v = s.find("maxItems"); v.is_number() && static_cast<double>(n) > v.as_number()) {
      if (report) add_issue("maxItems", "array has more than " + v.dump() + " items");
      A2UI_AFAIL();
    }
    if (SchemaNode u = s.find("uniqueItems"); u.as_bool()) {
      bool dup = false;
      for (size_t i = 0; i < n && !dup; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
          if (inst[i] == inst[j]) {
            dup = true;
            break;
          }
        }
      }
      if (dup) {
        if (report) add_issue("uniqueItems", "array items are not unique");
        A2UI_AFAIL();
      }
    }
    size_t prefix = 0;
    if (SchemaNode pi = s.find("prefixItems"); pi.is_array()) {
      prefix = std::min(pi.size(), n);
      for (size_t i = 0; i < prefix; ++i) {
        if (!descend_index(doc, pi.at(i), i, inst[i], report, depth, "prefixItems")) A2UI_AFAIL();
      }
      annot->items = std::max(annot->items, prefix);
    }
    if (SchemaNode items = s.find("items")) {
      for (size_t i = prefix; i < n; ++i) {
        if (!descend_index(doc, items, i, inst[i], report, depth, "items")) A2UI_AFAIL();
      }
      annot->all_items = true;
    }
    if (SchemaNode c = s.find("contains")) {
      size_t count = 0;
      for (size_t i = 0; i < n; ++i) {
        if (eval(doc, c, inst[i], nullptr, false, depth + 1)) {
          ++count;
          annot->item_idx.push_back(i);
        }
      }
      const SchemaNode mn = s.find("minContains");
      const SchemaNode mx = s.find("maxContains");
      const double min_c = mn.is_number() ? mn.as_number() : 1;
      if (static_cast<double>(count) < min_c) {
        if (report) add_issue("contains", "array does not contain enough matching items");
        A2UI_AFAIL();
      }
      if (mx.is_number() && static_cast<double>(count) > mx.as_number()) {
        if (report) add_issue("maxContains", "array contains too many matching items");
        A2UI_AFAIL();
      }
    }
#undef A2UI_AFAIL
    return ok;
  }

  const SchemaRegistry& reg_;
  const SchemaValidateOptions& opt_;

 public:
  /**
   * Where null is accepted under accept_null_properties, mirroring the
   * Swift core's allowingNullValues / acceptingNull schema rewrite:
   * Root is the arguments object (its properties become Value, its
   * allOf/anyOf/oneOf branches stay Root); a Value position accepts null
   * and passes Value on to its own properties and items. $ref targets and
   * other keywords are evaluated as written.
   */
  enum class NullScope : uint8_t { None, Root, Value };
  NullScope next_ = NullScope::None;

 private:
  NullScope scope_ = NullScope::None;
  NullScope combiner_scope() const { return scope_ == NullScope::Root ? NullScope::Root : NullScope::None; }
  struct ScopeGuard {
    ScopeGuard(NullScope* slot, NullScope value) : slot_(slot), saved_(*slot) { *slot = value; }
    ~ScopeGuard() { *slot_ = saved_; }
    NullScope* slot_;
    NullScope saved_;
  };
};

Status finish(Validator& v, bool ok, std::vector<ValidationIssue>* issues) {
  if (ok) return ok_status();
  if (v.issues.empty()) v.add_issue("schema", "value does not match the schema");
  const ValidationIssue& first = v.issues.front();
  std::string msg = "Schema validation failed at '" +
                    (first.instance_path.empty() ? std::string("/") : first.instance_path) +
                    "' (" + first.keyword + "): " + first.message;
  if (v.issues.size() > 1) msg += " (+" + std::to_string(v.issues.size() - 1) + " more)";
  Error e = make_error(v.recursion_hit ? ErrorKind::Recursion : ErrorKind::Validation, std::move(msg),
                       "VALIDATION_FAILED");
  e.details = issues_to_json(v.issues);
  if (issues) issues->insert(issues->end(), v.issues.begin(), v.issues.end());
  return e;
}

}  // namespace

Status validate_schema(const SchemaRegistry& registry, std::string_view doc_name,
                       SchemaNode schema, const Json& instance,
                       std::vector<ValidationIssue>* issues, const SchemaValidateOptions& options) {
  Validator v(registry, options);
  const int doc = registry.find_document(doc_name);
  if (options.accept_null_properties) v.next_ = Validator::NullScope::Root;
  const bool ok = v.eval(doc, schema, instance, nullptr, true, 0);
  return finish(v, ok, issues);
}

Status validate_ref(const SchemaRegistry& registry, std::string_view ref, const Json& instance,
                    std::vector<ValidationIssue>* issues, const SchemaValidateOptions& options) {
  Validator v(registry, options);
  std::string_view doc_part, pointer;
  split_ref(ref, &doc_part, &pointer);
  const int doc = registry.find_document(doc_part);
  int tdoc = doc;
  const SchemaNode target = doc < 0 ? SchemaNode() : v.resolve(doc, ref.substr(doc_part.size()), &tdoc);
  if (!target) {
    return make_error(ErrorKind::Generic, "Unresolvable schema reference '" + std::string(ref) + "'");
  }
  const bool ok = v.eval(tdoc, target, instance, nullptr, true, 0);
  return finish(v, ok, issues);
}

}  // namespace a2ui

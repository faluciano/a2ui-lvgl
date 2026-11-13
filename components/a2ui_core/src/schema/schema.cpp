// Copyright 2026 The a2ui-lgvl Authors
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

void SchemaRegistry::add(std::string name, std::shared_ptr<const Json> document) {
  add(std::move(name), std::move(document), nullptr);
}

void SchemaRegistry::add(std::string name, std::shared_ptr<const Json> document,
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

void SchemaRegistry::add_override(std::string ref, std::shared_ptr<const Json> schema) {
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

const Json* SchemaRegistry::resolve_pointer(int doc, std::string_view pointer) const {
  if (doc < 0 || static_cast<size_t>(doc) >= docs_.size()) return nullptr;
  const Doc& d = docs_[static_cast<size_t>(doc)];
  if (d.resolver) return d.resolver(pointer);
  return d.json ? resolve_json_pointer(*d.json, pointer) : nullptr;
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

std::string short_dump(const Json& v) {
  std::string s = v.dump();
  if (s.size() > 60) {
    s.resize(57);
    s += "...";
  }
  return s;
}

class Validator {
 public:
  Validator(const SchemaRegistry& reg, const SchemaValidateOptions& opt)
      : reg_(reg), opt_(opt) {}

  std::vector<ValidationIssue> issues;
  bool recursion_hit = false;
  bool unresolved_ref = false;
  std::string path;

  bool eval(int doc, const Json& schema, const Json& inst, Annot* annot, bool report, int depth) {
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
    if (const Json* ref = schema.find("$ref"); ref && ref->is_string()) {
      int tdoc = doc;
      const Json* target = resolve(doc, ref->as_string(), &tdoc);
      if (!target) {
        unresolved_ref = true;
        if (report) add_issue("$ref", "unresolvable reference '" + ref->as_string() + "'");
        A2UI_FAIL();
      } else {
        Annot sub;
        if (eval(tdoc, *target, inst, &sub, report, depth + 1)) {
          local.merge(sub);
        } else {
          A2UI_FAIL();
        }
      }
    }

    // --- type / enum / const
    if (const Json* t = schema.find("type")) {
      bool ok = false;
      if (t->is_string()) {
        ok = type_matches(t->as_string(), inst);
      } else if (t->is_array()) {
        for (const auto& e : t->as_array()) {
          if (e.is_string() && type_matches(e.as_string(), inst)) {
            ok = true;
            break;
          }
        }
      } else {
        ok = true;
      }
      if (!ok) {
        if (report) add_issue("type", "expected " + type_list(*t) + ", got " + type_name(inst));
        A2UI_FAIL();
      }
    }
    if (const Json* e = schema.find("enum"); e && e->is_array()) {
      bool ok = false;
      for (const auto& v : e->as_array()) {
        if (v == inst) {
          ok = true;
          break;
        }
      }
      if (!ok) {
        if (report) add_issue("enum", short_dump(inst) + " is not one of " + short_dump(*e));
        A2UI_FAIL();
      }
    }
    if (const Json* c = schema.find("const")) {
      if (!(*c == inst)) {
        if (report) add_issue("const", "expected " + short_dump(*c) + ", got " + short_dump(inst));
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
    if (const Json* all = schema.find("allOf"); all && all->is_array()) {
      for (const auto& sub : all->as_array()) {
        Annot a;
        if (eval(doc, sub, inst, &a, report, depth + 1)) {
          local.merge(a);
        } else {
          A2UI_FAIL();
        }
      }
    }
    if (const Json* any = schema.find("anyOf"); any && any->is_array()) {
      bool ok = false;
      for (const auto& sub : any->as_array()) {
        Annot a;
        if (eval(doc, sub, inst, &a, false, depth + 1)) {
          ok = true;
          local.merge(a);  // Every passing branch contributes annotations.
        }
      }
      if (!ok) {
        if (report) {
          add_issue("anyOf", "value does not match any allowed schema");
          explain_branches(doc, *any, inst, depth);
        }
        A2UI_FAIL();
      }
    }
    if (const Json* one = schema.find("oneOf"); one && one->is_array()) {
      size_t matched = 0;
      Annot first;
      for (const auto& sub : one->as_array()) {
        Annot a;
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
            explain_branches(doc, *one, inst, depth);
          } else {
            add_issue("oneOf", "value matches more than one schema");
          }
        }
        A2UI_FAIL();
      }
    }
    if (const Json* n = schema.find("not")) {
      Annot ignored;
      if (eval(doc, *n, inst, &ignored, false, depth + 1)) {
        if (report) add_issue("not", "value matches a schema it must not match");
        A2UI_FAIL();
      }
    }
    if (const Json* cond = schema.find("if")) {
      Annot a;
      const bool if_ok = eval(doc, *cond, inst, &a, false, depth + 1);
      const Json* branch = schema.find(if_ok ? "then" : "else");
      if (if_ok) local.merge(a);
      if (branch) {
        Annot b;
        if (eval(doc, *branch, inst, &b, report, depth + 1)) {
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
      if (const Json* u = schema.find("unevaluatedProperties")) {
        for (const auto& m : inst.as_object()) {
          if (local.has_prop(&m.first)) continue;
          if (!descend_key(doc, *u, m.first, m.second, report, depth, "unevaluatedProperties")) {
            A2UI_FAIL();
          }
          local.props.push_back(&m.first);
        }
      }
    }
    if (valid && inst.is_array()) {
      if (const Json* u = schema.find("unevaluatedItems")) {
        if (!local.all_items) {
          for (size_t i = local.items; i < inst.size(); ++i) {
            if (std::find(local.item_idx.begin(), local.item_idx.end(), i) != local.item_idx.end()) {
              continue;
            }
            if (!descend_index(doc, *u, i, inst[i], report, depth, "unevaluatedItems")) A2UI_FAIL();
          }
        }
        local.all_items = true;
      }
    }
#undef A2UI_FAIL

    if (valid && annot) annot->merge(local);
    return valid;
  }

  const Json* resolve(int doc, const std::string& ref, int* out_doc) {
    std::string_view doc_part, pointer;
    split_ref(ref, &doc_part, &pointer);
    if (!doc_part.empty() && is_meta_schema(doc_part)) {
      *out_doc = doc;
      return &true_schema();
    }
    const int target = doc_part.empty() ? doc : reg_.find_document(doc_part);
    if (target < 0) return nullptr;
    *out_doc = target;
    if (const Json* o = reg_.override_for(target, pointer)) return o;
    return reg_.resolve_pointer(target, pointer);
  }

  void add_issue(const char* keyword, std::string message) {
    if (issues.size() >= opt_.max_issues) return;
    issues.push_back({path, keyword, std::move(message)});
  }

 private:
  static std::string type_list(const Json& t) {
    if (t.is_string()) return t.as_string();
    std::string out;
    for (const auto& e : t.as_array()) {
      if (!out.empty()) out += " or ";
      out += e.as_string();
    }
    return out;
  }

  // When no branch of anyOf/oneOf matched, reports the issues of the single
  // branch that could apply, if exactly one does (by `type` or by `const`
  // members such as "@call"), to give a specific message.
  void explain_branches(int doc, const Json& branches, const Json& inst, int depth) {
    const Json* candidate = nullptr;
    int candidates = 0;
    for (const auto& b : branches.as_array()) {
      if (!plausible(doc, b, inst, 0)) continue;
      candidate = &b;
      ++candidates;
    }
    if (candidates == 1) {
      Annot ignored;
      eval(doc, *candidate, inst, &ignored, true, depth + 1);
    }
  }

  // A cheap guess whether `inst` was meant for branch `b`: its type agrees
  // and every `const`-valued property the branch requires is present.
  bool plausible(int doc, const Json& b, const Json& inst, int hops) {
    if (!b.is_object() || hops > 8) return false;
    if (const Json* ref = b.find("$ref"); ref && ref->is_string()) {
      int tdoc = doc;
      const Json* t = resolve(doc, ref->as_string(), &tdoc);
      if (t && !plausible(tdoc, *t, inst, hops + 1)) return false;
      if (!t) return false;
    }
    if (const Json* t = b.find("type"); t && t->is_string() && !type_matches(t->as_string(), inst)) {
      return false;
    }
    if (const Json* props = b.find("properties"); props && props->is_object() && inst.is_object()) {
      for (const auto& m : props->as_object()) {
        if (const Json* c = m.second.find("const")) {
          const Json* v = inst.find(m.first);
          if (v && !(*v == *c)) return false;
        }
      }
      if (const Json* req = b.find("required"); req && req->is_array()) {
        for (const auto& r : req->as_array()) {
          if (r.is_string() && !inst.contains(r.as_string())) return false;
        }
      }
    }
    if (const Json* all = b.find("allOf"); all && all->is_array()) {
      for (const auto& s : all->as_array()) {
        if (s.is_object() && !plausible(doc, s, inst, hops + 1)) return false;
      }
    }
    return true;
  }

  bool check_number(const Json& s, double x, bool report) {
    bool ok = true;
    auto num = [&](const char* k) -> const Json* {
      const Json* v = s.find(k);
      return v && v->is_number() ? v : nullptr;
    };
    if (const Json* v = num("minimum"); v && x < v->as_number()) {
      if (report) add_issue("minimum", number_to_string(x) + " is less than " + v->dump());
      ok = false;
    }
    if (const Json* v = num("maximum"); v && x > v->as_number()) {
      if (report) add_issue("maximum", number_to_string(x) + " is greater than " + v->dump());
      ok = false;
    }
    if (const Json* v = num("exclusiveMinimum"); v && x <= v->as_number()) {
      if (report) add_issue("exclusiveMinimum", number_to_string(x) + " must be greater than " + v->dump());
      ok = false;
    }
    if (const Json* v = num("exclusiveMaximum"); v && x >= v->as_number()) {
      if (report) add_issue("exclusiveMaximum", number_to_string(x) + " must be less than " + v->dump());
      ok = false;
    }
    if (const Json* v = num("multipleOf"); v && v->as_number() > 0) {
      const double q = x / v->as_number();
      if (!std::isfinite(q) || std::fabs(q - std::round(q)) > 1e-9) {
        if (report) add_issue("multipleOf", number_to_string(x) + " is not a multiple of " + v->dump());
        ok = false;
      }
    }
    return ok;
  }

  bool check_string(const Json& s, const std::string& str, bool report) {
    bool ok = true;
    const Json* min_len = s.find("minLength");
    const Json* max_len = s.find("maxLength");
    if ((min_len && min_len->is_number()) || (max_len && max_len->is_number())) {
      const double len = static_cast<double>(utf8_length(str));
      if (min_len && min_len->is_number() && len < min_len->as_number()) {
        if (report) add_issue("minLength", "string is shorter than " + min_len->dump());
        ok = false;
      }
      if (max_len && max_len->is_number() && len > max_len->as_number()) {
        if (report) add_issue("maxLength", "string is longer than " + max_len->dump());
        ok = false;
      }
    }
    if (const Json* p = s.find("pattern"); p && p->is_string()) {
      bool parsed = true;
      if (!pattern_search(p->as_string(), str, &parsed)) {
        if (report) {
          add_issue("pattern", parsed ? short_dump(Json(str)) + " does not match '" + p->as_string() + "'"
                                      : "unsupported pattern '" + p->as_string() + "'");
        }
        ok = false;
      }
    }
    return ok;
  }

  bool pattern_search(const std::string& pattern, const std::string& text, bool* parsed) {
    // UAX #31 identifier patterns are matched directly: they are the most
    // common pattern in the A2UI schemas (Extensions keys).
    if (pattern == "^[\\p{XID_Start}_][\\p{XID_Continue}]*$") {
      *parsed = true;
      return is_uax31_identifier(text);
    }
    return schema_pattern::search(pattern, text, parsed);
  }

  bool descend_key(int doc, const Json& sub, const std::string& key, const Json& value,
                   bool report, int depth, const char* keyword) {
    const size_t mark = path.size();
    path += '/';
    path += escape_pointer_token(key);
    const size_t before = issues.size();
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

  bool descend_index(int doc, const Json& sub, size_t index, const Json& value, bool report,
                     int depth, const char* keyword) {
    const size_t mark = path.size();
    path += '/';
    path += std::to_string(index);
    const bool ok = eval(doc, sub, value, nullptr, report, depth + 1);
    if (!ok && report && sub.is_bool() && !issues.empty() && issues.back().keyword == std::string("false")) {
      issues.back().keyword = keyword;
      issues.back().message = "item " + std::to_string(index) + " is not allowed";
    }
    path.resize(mark);
    return ok;
  }

  bool check_object(int doc, const Json& s, const Json& inst, Annot* annot, bool report, int depth) {
    bool ok = true;
#define A2UI_OFAIL()           \
  do {                         \
    ok = false;                \
    if (!report) return false; \
  } while (0)
    if (const Json* req = s.find("required"); req && req->is_array()) {
      for (const auto& r : req->as_array()) {
        if (r.is_string() && !inst.contains(r.as_string())) {
          if (report) add_issue("required", "missing required property '" + r.as_string() + "'");
          A2UI_OFAIL();
        }
      }
    }
    if (const Json* v = s.find("minProperties"); v && v->is_number() &&
        static_cast<double>(inst.size()) < v->as_number()) {
      if (report) add_issue("minProperties", "object has fewer than " + v->dump() + " properties");
      A2UI_OFAIL();
    }
    if (const Json* v = s.find("maxProperties"); v && v->is_number() &&
        static_cast<double>(inst.size()) > v->as_number()) {
      if (report) add_issue("maxProperties", "object has more than " + v->dump() + " properties");
      A2UI_OFAIL();
    }
    if (const Json* dr = s.find("dependentRequired"); dr && dr->is_object()) {
      for (const auto& m : dr->as_object()) {
        if (!inst.contains(m.first) || !m.second.is_array()) continue;
        for (const auto& r : m.second.as_array()) {
          if (r.is_string() && !inst.contains(r.as_string())) {
            if (report) {
              add_issue("dependentRequired",
                        "property '" + r.as_string() + "' is required with '" + m.first + "'");
            }
            A2UI_OFAIL();
          }
        }
      }
    }
    if (const Json* ds = s.find("dependentSchemas"); ds && ds->is_object()) {
      for (const auto& m : ds->as_object()) {
        if (!inst.contains(m.first)) continue;
        Annot a;
        if (eval(doc, m.second, inst, &a, report, depth + 1)) {
          annot->merge(a);
        } else {
          A2UI_OFAIL();
        }
      }
    }
    if (const Json* pn = s.find("propertyNames")) {
      for (const auto& m : inst.as_object()) {
        const Json key(m.first);
        const size_t before = issues.size();
        if (!eval(doc, *pn, key, nullptr, false, depth + 1)) {
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

    const Json* props = s.find("properties");
    const Json* pattern_props = s.find("patternProperties");
    const Json* additional = s.find("additionalProperties");
    if (props || pattern_props || additional) {
      for (const auto& m : inst.as_object()) {
        bool matched = false;
        if (props && props->is_object()) {
          if (const Json* sub = props->find(m.first)) {
            matched = true;
            if (!descend_key(doc, *sub, m.first, m.second, report, depth, "properties")) A2UI_OFAIL();
          }
        }
        if (pattern_props && pattern_props->is_object()) {
          for (const auto& pp : pattern_props->as_object()) {
            bool parsed = true;
            if (!pattern_search(pp.first, m.first, &parsed)) continue;
            matched = true;
            if (!descend_key(doc, pp.second, m.first, m.second, report, depth, "patternProperties")) {
              A2UI_OFAIL();
            }
          }
        }
        if (!matched && additional) {
          matched = true;
          if (!descend_key(doc, *additional, m.first, m.second, report, depth, "additionalProperties")) {
            A2UI_OFAIL();
          }
        }
        if (matched) annot->props.push_back(&m.first);
      }
    }
#undef A2UI_OFAIL
    return ok;
  }

  bool check_array(int doc, const Json& s, const Json& inst, Annot* annot, bool report, int depth) {
    bool ok = true;
#define A2UI_AFAIL()           \
  do {                         \
    ok = false;                \
    if (!report) return false; \
  } while (0)
    const size_t n = inst.size();
    if (const Json* v = s.find("minItems"); v && v->is_number() && static_cast<double>(n) < v->as_number()) {
      if (report) add_issue("minItems", "array has fewer than " + v->dump() + " items");
      A2UI_AFAIL();
    }
    if (const Json* v = s.find("maxItems"); v && v->is_number() && static_cast<double>(n) > v->as_number()) {
      if (report) add_issue("maxItems", "array has more than " + v->dump() + " items");
      A2UI_AFAIL();
    }
    if (const Json* u = s.find("uniqueItems"); u && u->as_bool()) {
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
    if (const Json* pi = s.find("prefixItems"); pi && pi->is_array()) {
      prefix = std::min(pi->size(), n);
      for (size_t i = 0; i < prefix; ++i) {
        if (!descend_index(doc, (*pi)[i], i, inst[i], report, depth, "prefixItems")) A2UI_AFAIL();
      }
      annot->items = std::max(annot->items, prefix);
    }
    if (const Json* items = s.find("items")) {
      for (size_t i = prefix; i < n; ++i) {
        if (!descend_index(doc, *items, i, inst[i], report, depth, "items")) A2UI_AFAIL();
      }
      annot->all_items = true;
    }
    if (const Json* c = s.find("contains")) {
      size_t count = 0;
      for (size_t i = 0; i < n; ++i) {
        if (eval(doc, *c, inst[i], nullptr, false, depth + 1)) {
          ++count;
          annot->item_idx.push_back(i);
        }
      }
      const Json* mn = s.find("minContains");
      const Json* mx = s.find("maxContains");
      const double min_c = mn && mn->is_number() ? mn->as_number() : 1;
      if (static_cast<double>(count) < min_c) {
        if (report) add_issue("contains", "array does not contain enough matching items");
        A2UI_AFAIL();
      }
      if (mx && mx->is_number() && static_cast<double>(count) > mx->as_number()) {
        if (report) add_issue("maxContains", "array contains too many matching items");
        A2UI_AFAIL();
      }
    }
#undef A2UI_AFAIL
    return ok;
  }

  const SchemaRegistry& reg_;
  const SchemaValidateOptions& opt_;
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
                       const Json& schema, const Json& instance,
                       std::vector<ValidationIssue>* issues, const SchemaValidateOptions& options) {
  Validator v(registry, options);
  const int doc = registry.find_document(doc_name);
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
  const Json* target = doc < 0 ? nullptr : v.resolve(doc, std::string(ref.substr(doc_part.size())), &tdoc);
  if (!target) {
    return make_error(ErrorKind::Generic, "Unresolvable schema reference '" + std::string(ref) + "'");
  }
  const bool ok = v.eval(tdoc, *target, instance, nullptr, true, 0);
  return finish(v, ok, issues);
}

}  // namespace a2ui

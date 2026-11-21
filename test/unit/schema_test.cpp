// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the JSON Schema interpreter (a2ui/schema.h).

#include <memory>
#include <string>
#include <vector>

#include "a2ui/json.h"
#include "a2ui/schema.h"
#include "test_main.h"

using a2ui::Json;
using a2ui::SchemaRegistry;
using a2ui::ValidationIssue;

namespace {

Json J(const char* text) {
  auto r = Json::parse(text);
  if (!r.ok) unit::report_failure(__FILE__, __LINE__, std::string("bad test JSON: ") + text);
  return r.value;
}

std::shared_ptr<const Json> doc(const char* text) { return std::make_shared<const Json>(J(text)); }

// Validates `instance` against `schema` as a standalone document "test.json".
bool valid(const char* schema, const char* instance, std::vector<ValidationIssue>* issues = nullptr) {
  SchemaRegistry reg;
  auto d = doc(schema);
  reg.add("test.json", d);
  return a2ui::validate_schema(reg, "test.json", *d, J(instance), issues).ok();
}

bool matches(const char* pattern, const char* text) { return a2ui::schema_pattern::search(pattern, text); }

}  // namespace

TEST(schema_type_enum_const) {
  EXPECT_TRUE(valid(R"({"type":"string"})", R"("x")"));
  EXPECT_TRUE(!valid(R"({"type":"string"})", "1"));
  EXPECT_TRUE(valid(R"({"type":["string","null"]})", "null"));
  EXPECT_TRUE(valid(R"({"type":"integer"})", "3"));
  EXPECT_TRUE(!valid(R"({"type":"integer"})", "3.5"));
  EXPECT_TRUE(valid(R"({"type":"number"})", "3.5"));
  EXPECT_TRUE(!valid(R"({"type":"object"})", "[]"));
  EXPECT_TRUE(valid(R"({"enum":["a",1,{"k":[1]}]})", R"({"k":[1]})"));
  EXPECT_TRUE(!valid(R"({"enum":["a",1]})", R"("b")"));
  EXPECT_TRUE(valid(R"({"const":{"a":1,"b":2}})", R"({"b":2,"a":1})"));
  EXPECT_TRUE(!valid(R"({"const":"root"})", "null"));
  EXPECT_TRUE(valid("true", "1"));
  EXPECT_TRUE(!valid("false", "1"));
  EXPECT_TRUE(valid("{}", R"({"anything":[1,2]})"));
}

TEST(schema_numbers_and_strings) {
  EXPECT_TRUE(!valid(R"({"minimum":1})", "0"));
  EXPECT_TRUE(valid(R"({"minimum":1})", "1"));
  EXPECT_TRUE(!valid(R"({"exclusiveMinimum":1})", "1"));
  EXPECT_TRUE(!valid(R"({"maximum":10})", "11"));
  EXPECT_TRUE(!valid(R"({"exclusiveMaximum":10})", "10"));
  EXPECT_TRUE(valid(R"({"multipleOf":0.5})", "2.5"));
  EXPECT_TRUE(!valid(R"({"multipleOf":2})", "3"));
  EXPECT_TRUE(valid(R"({"minimum":1})", R"("not a number")"));  // Only numbers are checked.
  // Lengths count code points, not bytes.
  EXPECT_TRUE(valid(R"({"maxLength":2})", "\"\xC3\xA9\xF0\x9F\x98\x80\""));
  EXPECT_TRUE(!valid(R"({"minLength":3})", R"("ab")"));
  EXPECT_TRUE(valid(R"({"pattern":"^#[0-9a-fA-F]{6}$"})", R"("#1a2B3c")"));
  EXPECT_TRUE(!valid(R"({"pattern":"^#[0-9a-fA-F]{6}$"})", R"("not-a-color")"));
  EXPECT_TRUE(valid(R"({"format":"uri"})", R"("not checked")"));
}

TEST(schema_objects) {
  const char* s = R"({
    "type":"object",
    "properties":{"a":{"type":"string"},"b":{"type":"number"}},
    "required":["a"],
    "additionalProperties":false
  })";
  EXPECT_TRUE(valid(s, R"({"a":"x","b":1})"));
  EXPECT_TRUE(!valid(s, R"({"b":1})"));
  EXPECT_TRUE(!valid(s, R"({"a":"x","c":1})"));
  EXPECT_TRUE(!valid(s, R"({"a":1})"));
  EXPECT_TRUE(valid(R"({"patternProperties":{"^x_":{"type":"number"}},"additionalProperties":false})",
                    R"({"x_a":1})"));
  EXPECT_TRUE(!valid(R"({"patternProperties":{"^x_":{"type":"number"}},"additionalProperties":false})",
                     R"({"y":1})"));
  EXPECT_TRUE(!valid(R"j({"propertyNames":{"not":{"pattern":"^@([^@]|$)"}}})j", R"({"@path":1})"));
  EXPECT_TRUE(valid(R"j({"propertyNames":{"not":{"pattern":"^@([^@]|$)"}}})j", R"({"@@path":1})"));
  EXPECT_TRUE(!valid(R"j({"propertyNames":{"not":{"pattern":"^@([^@]|$)"}}})j", R"({"@":1})"));
  EXPECT_TRUE(!valid(R"({"minProperties":1})", "{}"));
  EXPECT_TRUE(!valid(R"({"dependentRequired":{"a":["b"]}})", R"({"a":1})"));
  // Extensions keys: the UAX #31 pattern.
  const char* ext = R"({"patternProperties":{"^[\\p{XID_Start}_][\\p{XID_Continue}]*$":{}},"additionalProperties":false})";
  EXPECT_TRUE(valid(ext, "{\"\xC3\xBCn\xC3\xAF\x63ode\":1,\"_p\":2}"));
  EXPECT_TRUE(!valid(ext, R"({"has-dash":1})"));
  EXPECT_TRUE(!valid(ext, R"({"1st":1})"));
  EXPECT_TRUE(!valid(ext, R"({"":1})"));
}

TEST(schema_unevaluated_properties_follow_annotations) {
  // allOf members contribute evaluated properties.
  const char* s = R"({
    "allOf":[{"properties":{"a":{}}},{"properties":{"b":{}}}],
    "unevaluatedProperties":false
  })";
  EXPECT_TRUE(valid(s, R"({"a":1,"b":2})"));
  EXPECT_TRUE(!valid(s, R"({"a":1,"c":2})"));
  // Only the passing oneOf branch contributes.
  const char* one = R"({
    "oneOf":[{"properties":{"a":{"type":"string"}},"required":["a"]},
             {"properties":{"b":{"type":"string"}},"required":["b"]}],
    "unevaluatedProperties":false
  })";
  EXPECT_TRUE(valid(one, R"({"a":"x"})"));
  EXPECT_TRUE(!valid(one, R"({"a":"x","b":1})"));
  // A failed `if` contributes nothing; `then` does.
  const char* cond = R"({
    "if":{"properties":{"kind":{"const":"x"}},"required":["kind"]},
    "then":{"properties":{"x":{}}},
    "unevaluatedProperties":false
  })";
  EXPECT_TRUE(valid(cond, R"({"kind":"x","x":1})"));
  EXPECT_TRUE(!valid(cond, R"({"kind":"y","x":1})"));
  // Annotations flow through $ref.
  const char* ref = R"({
    "$defs":{"Base":{"properties":{"id":{"type":"string"}}}},
    "allOf":[{"$ref":"#/$defs/Base"}],
    "properties":{"own":{}},
    "unevaluatedProperties":false
  })";
  EXPECT_TRUE(valid(ref, R"({"id":"a","own":1})"));
  EXPECT_TRUE(!valid(ref, R"({"id":"a","other":1})"));
  // Annotations do not leak out of `not`.
  EXPECT_TRUE(!valid(R"({"not":{"properties":{"z":{"type":"number"}},"required":["q"]},"unevaluatedProperties":false})",
                     R"({"z":1})"));
}

TEST(schema_combinators) {
  EXPECT_TRUE(valid(R"({"anyOf":[{"type":"string"},{"type":"number"}]})", "1"));
  EXPECT_TRUE(!valid(R"({"anyOf":[{"type":"string"},{"type":"number"}]})", "true"));
  EXPECT_TRUE(!valid(R"({"oneOf":[{"type":"number"},{"minimum":0}]})", "1"));  // Both match.
  EXPECT_TRUE(valid(R"({"oneOf":[{"type":"number"},{"type":"string"}]})", "1"));
  EXPECT_TRUE(!valid(R"({"not":{"const":"Surface"}})", R"("Surface")"));
  EXPECT_TRUE(valid(R"({"if":{"type":"number"},"then":{"minimum":0},"else":{"type":"string"}})", R"("s")"));
  EXPECT_TRUE(!valid(R"({"if":{"type":"number"},"then":{"minimum":0},"else":{"type":"string"}})", "-1"));
  EXPECT_TRUE(!valid(R"({"if":{"type":"number"},"then":{"minimum":0},"else":{"type":"string"}})", "true"));
}

TEST(schema_arrays) {
  EXPECT_TRUE(valid(R"({"items":{"type":"string"}})", R"(["a","b"])"));
  EXPECT_TRUE(!valid(R"({"items":{"type":"string"}})", R"(["a",1])"));
  EXPECT_TRUE(valid(R"({"prefixItems":[{"type":"number"}],"items":{"type":"string"}})", R"([1,"a"])"));
  EXPECT_TRUE(!valid(R"({"prefixItems":[{"type":"number"}],"items":false})", R"([1,"a"])"));
  EXPECT_TRUE(!valid(R"({"prefixItems":[{}],"unevaluatedItems":false})", "[1,2]"));
  EXPECT_TRUE(valid(R"({"contains":{"const":"@call"}})", R"(["x","@call"])"));
  EXPECT_TRUE(!valid(R"({"contains":{"const":"@call"}})", R"(["x"])"));
  EXPECT_TRUE(!valid(R"({"minItems":1})", "[]"));
  EXPECT_TRUE(!valid(R"({"maxItems":1})", "[1,2]"));
  EXPECT_TRUE(!valid(R"({"uniqueItems":true})", R"([{"a":1},{"a":1}])"));
  EXPECT_TRUE(valid(R"({"uniqueItems":true})", R"([{"a":1},{"a":2}])"));
}

TEST(schema_issue_paths) {
  std::vector<ValidationIssue> issues;
  EXPECT_TRUE(!valid(R"({"properties":{"list":{"items":{"properties":{"a/b":{"type":"string"}}}}}})",
                     R"({"list":[{"a/b":"ok"},{"a/b":3}]})", &issues));
  EXPECT_EQ(issues.size(), size_t(1));
  if (!issues.empty()) {
    EXPECT_EQ(issues[0].instance_path, std::string("/list/1/a~1b"));
    EXPECT_EQ(issues[0].keyword, std::string("type"));
  }
  issues.clear();
  EXPECT_TRUE(!valid(R"({"required":["x","y"]})", "{}", &issues));
  EXPECT_EQ(issues.size(), size_t(2));
  issues.clear();
  EXPECT_TRUE(!valid(R"({"properties":{"a":false}})", R"({"a":1})", &issues));
  EXPECT_TRUE(!issues.empty() && issues[0].keyword == "properties" && issues[0].instance_path == "/a");
}

TEST(schema_cross_document_refs) {
  SchemaRegistry reg;
  auto common = doc(R"({"$id":"https://a2ui.org/specification/v1_0/common_types.json",
                        "$defs":{"Str":{"type":"string"},
                                 "Pair":{"properties":{"x":{"$ref":"#/$defs/Str"},
                                                       "y":{"$ref":"catalog.json#/$defs/Y"}}}}})");
  auto cat = doc(R"({"$defs":{"Y":{"type":"number"}},
                     "components":{"A":{"properties":{"p":{"$ref":"common_types.json#/$defs/Pair"}}}}})");
  reg.add("common_types.json", common);
  reg.add("catalog.json", cat);
  // Relative, absolute ($id) and basename-only references all resolve.
  EXPECT_TRUE(a2ui::validate_ref(reg, "common_types.json#/$defs/Str", J(R"("s")")).ok());
  EXPECT_TRUE(a2ui::validate_ref(reg, "https://a2ui.org/specification/v1_0/common_types.json#/$defs/Str",
                                 J(R"("s")")).ok());
  EXPECT_TRUE(a2ui::validate_ref(reg, "https://example.com/x/common_types.json#/$defs/Str", J(R"("s")")).ok());
  // "#/..." inside common_types resolves in common_types; catalog.json in the catalog.
  EXPECT_TRUE(a2ui::validate_ref(reg, "catalog.json#/components/A", J(R"({"p":{"x":"a","y":1}})")).ok());
  EXPECT_TRUE(!a2ui::validate_ref(reg, "catalog.json#/components/A", J(R"({"p":{"x":"a","y":"1"}})")).ok());
  EXPECT_TRUE(!a2ui::validate_ref(reg, "catalog.json#/components/A", J(R"({"p":{"x":1}})")).ok());
  // Unknown target: an error, not a crash.
  EXPECT_TRUE(!a2ui::validate_ref(reg, "missing.json#/x", J("1")).ok());
  auto st = a2ui::validate_schema(reg, "catalog.json", J(R"({"$ref":"#/$defs/Nope"})"), J("1"));
  EXPECT_TRUE(!st.ok() && st.error().is(a2ui::ErrorKind::Validation));
  // The 2020-12 meta-schema stands for "any schema".
  EXPECT_TRUE(a2ui::validate_schema(reg, "catalog.json",
                                    J(R"({"$ref":"https://json-schema.org/draft/2020-12/schema"})"),
                                    J(R"({"type":"object"})")).ok());
}

TEST(schema_overrides_and_resolvers) {
  SchemaRegistry reg;
  auto common = doc(R"({"$defs":{"Call":{"const":"strict"},"Use":{"$ref":"#/$defs/Call"}}})");
  reg.add("common_types.json", common);
  EXPECT_TRUE(!a2ui::validate_ref(reg, "common_types.json#/$defs/Use", J(R"("loose")")).ok());
  reg.add_override("common_types.json#/$defs/Call", doc(R"({"type":"string"})"));
  EXPECT_TRUE(a2ui::validate_ref(reg, "common_types.json#/$defs/Use", J(R"("loose")")).ok());

  // A resolver serves pointers of a document that is not one Json tree.
  auto part = doc(R"({"type":"number"})");
  reg.add("virtual.json", nullptr, [part](std::string_view p) -> const Json* {
    return p == "/entries/n" ? part.get() : nullptr;
  });
  EXPECT_TRUE(a2ui::validate_ref(reg, "virtual.json#/entries/n", J("1")).ok());
  EXPECT_TRUE(!a2ui::validate_ref(reg, "virtual.json#/entries/n", J(R"("1")")).ok());
}

TEST(schema_recursion_is_bounded) {
  SchemaRegistry reg;
  auto d = doc(R"({"$defs":{"Loop":{"$ref":"#/$defs/Loop"}}})");
  reg.add("loop.json", d);
  auto st = a2ui::validate_ref(reg, "loop.json#/$defs/Loop", J("1"));
  EXPECT_TRUE(!st.ok() && st.error().kind == a2ui::ErrorKind::Recursion);

  // A legitimately recursive schema validates nested data up to the limit.
  auto tree = doc(R"({"$defs":{"Node":{"type":"object","properties":{"kids":{"items":{"$ref":"#/$defs/Node"}}}}}})");
  reg.add("tree.json", tree);
  EXPECT_TRUE(a2ui::validate_ref(reg, "tree.json#/$defs/Node", J(R"({"kids":[{"kids":[{}]}]})")).ok());
  std::string deep = "{}";
  for (int i = 0; i < 60; ++i) deep = "{\"kids\":[" + deep + "]}";
  auto r = Json::parse(deep, 256);
  a2ui::SchemaValidateOptions opt;
  opt.max_depth = 40;
  auto deep_status = a2ui::validate_ref(reg, "tree.json#/$defs/Node", r.value, nullptr, opt);
  EXPECT_TRUE(!deep_status.ok() && deep_status.error().kind == a2ui::ErrorKind::Recursion);
}

TEST(schema_json_pointer) {
  const Json d = J(R"({"a":{"b/c":[10,20],"m~n":1,"%25":2}})");
  EXPECT_TRUE(a2ui::resolve_json_pointer(d, "") == &d);
  const Json* v = a2ui::resolve_json_pointer(d, "/a/b~1c/1");
  EXPECT_TRUE(v && v->as_number() == 20);
  v = a2ui::resolve_json_pointer(d, "/a/m~0n");
  EXPECT_TRUE(v && v->as_number() == 1);
  v = a2ui::resolve_json_pointer(d, "/a/%2525");
  EXPECT_TRUE(v && v->as_number() == 2);
  EXPECT_TRUE(a2ui::resolve_json_pointer(d, "/a/b~1c/01") == nullptr);
  EXPECT_TRUE(a2ui::resolve_json_pointer(d, "/a/b~1c/2") == nullptr);
  EXPECT_TRUE(a2ui::resolve_json_pointer(d, "/x") == nullptr);
  EXPECT_EQ(a2ui::escape_pointer_token("a/b~c"), std::string("a~1b~0c"));
}

TEST(schema_pattern_matcher) {
  EXPECT_TRUE(matches("^#[0-9a-fA-F]{6}$", "#00ffAA"));
  EXPECT_TRUE(!matches("^#[0-9a-fA-F]{6}$", "#00ffAA0"));
  EXPECT_TRUE(matches("b+", "abbbc"));  // Search, not full match.
  EXPECT_TRUE(!matches("^b+$", "abbbc"));
  EXPECT_TRUE(matches("^@([^@]|$)", "@"));
  EXPECT_TRUE(matches("^@([^@]|$)", "@path"));
  EXPECT_TRUE(!matches("^@([^@]|$)", "@@path"));
  EXPECT_TRUE(matches("^(?:ab|cd)*e$", "abcdabe"));
  EXPECT_TRUE(!matches("^(?:ab|cd)*e$", "abce"));
  EXPECT_TRUE(matches("^a{2,3}$", "aaa"));
  EXPECT_TRUE(!matches("^a{2,3}$", "aaaa"));
  EXPECT_TRUE(matches("^a.?c$", "ac"));
  EXPECT_TRUE(matches("^\\d+\\.\\d+$", "1.25"));
  EXPECT_TRUE(!matches("^\\d+\\.\\d+$", "1x25"));
  EXPECT_TRUE(matches("^\\w+\\s\\w+$", "hello world"));
  EXPECT_TRUE(matches("^[^\\s]+$", "no_spaces"));
  EXPECT_TRUE(matches("^a+?b$", "aaab"));
  EXPECT_TRUE(matches("\\bfoo\\b", "a foo b"));
  EXPECT_TRUE(!matches("\\bfoo\\b", "afoob"));
  EXPECT_TRUE(matches("^[\\p{XID_Start}_][\\p{XID_Continue}]*$", "Component_\xE2\x84\x93"));
  EXPECT_TRUE(!matches("^[\\p{XID_Start}_][\\p{XID_Continue}]*$", "a\xE2\x80\x93" "b"));  // En dash.
  EXPECT_TRUE(matches("^\\u00e9$", "\xC3\xA9"));
  EXPECT_TRUE(matches("^.$", "\xF0\x9F\x98\x80"));  // One code point.
  // The catalog_definition.json semver pattern.
  const char* semver =
      "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)(?:\\.(0|[1-9][0-9]*))?(?:-((?:0|[1-9][0-9]*|[0-9]*[a-zA-Z-][0-9a-zA-Z-]*)"
      "(?:\\.(?:0|[1-9][0-9]*|[0-9]*[a-zA-Z-][0-9a-zA-Z-]*))*))?(?:\\+([0-9a-zA-Z-]+(?:\\.[0-9a-zA-Z-]+)*))?$";
  EXPECT_TRUE(matches(semver, "1.0"));
  EXPECT_TRUE(matches(semver, "0.9.1-rc.1+build.5"));
  EXPECT_TRUE(!matches(semver, "01.0"));
  EXPECT_TRUE(!matches(semver, "v1.0"));
  // Unsupported syntax and runaway backtracking fail closed.
  bool ok = true;
  EXPECT_TRUE(!a2ui::schema_pattern::search("(?=a)", "a", &ok));
  EXPECT_TRUE(!ok);
  ok = true;
  EXPECT_TRUE(!a2ui::schema_pattern::search("^(a|a)*(a|a)*(a|a)*b$", std::string(60, 'a').c_str(), &ok));
  EXPECT_TRUE(!ok);
  ok = true;
  EXPECT_TRUE(!a2ui::schema_pattern::search("[a-", "a", &ok));
  EXPECT_TRUE(!ok);
  // Long single-character runs are matched without deep recursion.
  EXPECT_TRUE(matches("^[a-z]*$", std::string(20000, 'q').c_str()));
}

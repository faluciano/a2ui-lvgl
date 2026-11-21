// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for catalog loading (Catalog::from_json and friends) and
// single-catalog payload validation (a2ui/validation.h).

#include <string>
#include <vector>

#include "a2ui/catalog.h"
#include "a2ui/spec_documents.h"
#include "a2ui/validation.h"
#include "test_main.h"

using a2ui::Catalog;
using a2ui::CatalogPtr;
using a2ui::ErrorKind;
using a2ui::Json;
using a2ui::PropertyKind;

namespace {

Json J(const char* text) {
  auto r = Json::parse(text);
  if (!r.ok) unit::report_failure(__FILE__, __LINE__, std::string("bad test JSON: ") + text);
  return r.value;
}

CatalogPtr basic() {
  static CatalogPtr c = Catalog::from_json(*a2ui::spec::parsed(a2ui::spec::Document::BasicCatalogV1)).value();
  return c;
}

a2ui::Status load_error(const char* doc) {
  auto r = Catalog::from_json(J(doc));
  if (r.ok()) return a2ui::Status();
  return r.error();
}

bool component_ok(const Catalog& cat, const char* comp, const a2ui::ComponentValidationOptions& opt = {}) {
  const Json c = J(comp);
  const a2ui::ComponentApi* api = cat.find_component(c["component"].as_string());
  if (!api) return false;
  return a2ui::validate_component_properties(cat, *api, c, nullptr, opt).ok();
}

const char* kSmall = R"({
  "catalogId": "test:small",
  "protocolVersion": "1.0",
  "components": {
    "Label": {
      "type": "object",
      "allOf": [{"$ref": "common_types.json#/$defs/Checkable"}],
      "properties": {
        "component": {"const": "Label"},
        "text": {"$ref": "common_types.json#/$defs/DynamicString"},
        "size": {"$ref": "#/$defs/Size"},
        "next": {"$ref": "common_types.json#/$defs/ComponentId"},
        "onTap": {"$ref": "common_types.json#/$defs/Action"},
        "catalogId": {"type": "string"}
      },
      "required": ["component", "text"],
      "metadata": {"extensions": {"vendor_tag": 1}}
    }
  },
  "functions": {
    "upper": {
      "type": "object",
      "returnType": "string",
      "properties": {
        "@call": {"const": "upper"},
        "args": {
          "type": "object",
          "properties": {"value": {"$ref": "common_types.json#/$defs/DynamicString"}},
          "required": ["value"],
          "unevaluatedProperties": false
        }
      },
      "required": ["@call", "args"]
    },
    "secret": {
      "type": "object",
      "returnType": "string",
      "allowedCallers": "agentOnly",
      "properties": {"@call": {"const": "secret"}, "args": {"type": "object"}},
      "required": ["@call"]
    }
  },
  "$defs": {"Size": {"type": "number", "minimum": 1}}
})";

CatalogPtr small() {
  static CatalogPtr c = Catalog::from_json(J(kSmall)).value();
  return c;
}

}  // namespace

TEST(catalog_normalize_protocol_version) {
  EXPECT_EQ(a2ui::normalize_protocol_version("1.0"), std::string("1.0"));
  EXPECT_EQ(a2ui::normalize_protocol_version("v1.0"), std::string("1.0"));
  EXPECT_EQ(a2ui::normalize_protocol_version("1.0.2-rc.1"), std::string("1.0"));
  EXPECT_EQ(a2ui::normalize_protocol_version("v0.9.1"), std::string("0.9"));
  EXPECT_EQ(a2ui::normalize_protocol_version("0.8"), std::string("0.8"));
  EXPECT_EQ(a2ui::normalize_protocol_version("2.0"), std::string(""));
  EXPECT_EQ(a2ui::normalize_protocol_version("1"), std::string(""));
}

TEST(catalog_loads_basic_catalog) {
  CatalogPtr c = basic();
  EXPECT_TRUE(c != nullptr);
  if (!c) return;
  EXPECT_EQ(c->id(), std::string("https://a2ui.org/specification/v1_0/catalogs/basic/catalog.json"));
  EXPECT_EQ(c->protocol_version(), std::string("1.0"));
  EXPECT_EQ(c->components().size(), size_t(18));
  EXPECT_EQ(c->functions().size(), size_t(14));
  const a2ui::ComponentApi* button = c->find_component("Button");
  EXPECT_TRUE(button != nullptr);
  if (button) {
    EXPECT_TRUE(button->kind_of("child") == PropertyKind::ComponentId);
    EXPECT_TRUE(button->kind_of("action") == PropertyKind::Action);
    EXPECT_TRUE(button->kind_of("@checks") == PropertyKind::Checks);
    EXPECT_TRUE(button->kind_of("variant") == PropertyKind::Static);
    EXPECT_EQ(button->required.size(), size_t(2));
  }
  const a2ui::ComponentApi* row = c->find_component("Row");
  EXPECT_TRUE(row && row->kind_of("children") == PropertyKind::ChildList);
  const a2ui::ComponentApi* text = c->find_component("Text");
  EXPECT_TRUE(text && text->kind_of("text") == PropertyKind::DynamicString);
  const a2ui::FunctionDecl* open = c->find_function("openUrl");
  EXPECT_TRUE(open && open->api.requires_user_activation && open->api.return_type == "void");
  EXPECT_TRUE(open && open->api.args_schema["properties"].contains("url"));
  // Definitions were moved out of the source document, not copied.
  EXPECT_TRUE(c->source()["components"].empty());
  EXPECT_TRUE(c->source()["functions"].empty());
  // to_json puts them back.
  const Json out = c->to_json();
  EXPECT_TRUE(out == *a2ui::spec::parsed(a2ui::spec::Document::BasicCatalogV1));
}

TEST(catalog_property_kinds_and_metadata) {
  CatalogPtr c = small();
  EXPECT_TRUE(c != nullptr);
  if (!c) return;
  const a2ui::ComponentApi* label = c->find_component("Label");
  EXPECT_TRUE(label != nullptr);
  if (!label) return;
  EXPECT_TRUE(label->kind_of("text") == PropertyKind::DynamicString);
  EXPECT_TRUE(label->kind_of("size") == PropertyKind::Static);
  EXPECT_TRUE(label->kind_of("next") == PropertyKind::ComponentId);
  EXPECT_TRUE(label->kind_of("onTap") == PropertyKind::Action);
  EXPECT_TRUE(label->kind_of("@checks") == PropertyKind::Checks);
  EXPECT_TRUE(label->required == std::vector<std::string>{"text"});
  EXPECT_TRUE(label->metadata == J(R"({"extensions":{"vendor_tag":1}})"));
  const a2ui::FunctionDecl* secret = c->find_function("secret");
  EXPECT_TRUE(secret && secret->api.allowed_callers == a2ui::AllowedCallers::AgentOnly);
}

TEST(catalog_load_errors) {
  auto e = load_error(R"({"components":{}})");
  EXPECT_TRUE(!e.ok() && e.error().kind == ErrorKind::Catalog);
  e = load_error(R"({"catalogId":"x","protocolVersion":"3.1"})");
  EXPECT_TRUE(!e.ok() && e.error().kind == ErrorKind::Catalog);
  e = load_error(R"({"catalogId":"x","protocolVersion":"1.0","components":{"My-Comp":{}}})");
  EXPECT_TRUE(!e.ok() && e.error().message.find("Invalid UAX #31 component identifier") != std::string::npos);
  e = load_error(R"({"catalogId":"x","protocolVersion":"1.0","components":{"Surface":{}}})");
  EXPECT_TRUE(!e.ok() && e.error().kind == ErrorKind::Catalog);
  e = load_error(R"({"catalogId":"x","protocolVersion":"1.0","components":{"A":{"properties":{"bad name":{}}}}})");
  EXPECT_TRUE(!e.ok() && e.error().kind == ErrorKind::Catalog);
  e = load_error(R"({"catalogId":"x","protocolVersion":"1.0","components":{"A":{"allOf":[{"properties":{"x-y":{}}}]}}})");
  EXPECT_TRUE(!e.ok() && e.error().kind == ErrorKind::Catalog);
  e = load_error(R"({"catalogId":"x","protocolVersion":"1.0","functions":{"@index":{"returnType":"number"}}})");
  EXPECT_TRUE(!e.ok() && e.error().kind == ErrorKind::Catalog);
  e = load_error(R"({"catalogId":"x","protocolVersion":"1.0","functions":{"f":{"returnType":"void","parameters":{"9x":{}}}}})");
  EXPECT_TRUE(!e.ok() && e.error().message.find("argument identifier") != std::string::npos);
  e = load_error(R"({"catalogId":"x","protocolVersion":"1.0","functions":{"f":{"returnType":"void",
      "allowedCallers":"agentOnly","requiresUserActivation":true}}})");
  EXPECT_TRUE(!e.ok() && e.error().message.find("requiresUserActivation") != std::string::npos);
  e = load_error(R"({"catalogId":"x","protocolVersion":"1.0","components":{"A":{"$ref":"other.json#/components/B"}}})");
  EXPECT_TRUE(!e.ok() && e.error().message.find("Unsupported external $ref") != std::string::npos);
  e = load_error(R"({"catalogId":"x","protocolVersion":"1.0","components":{"A":{"metadata":{"extensions":{"bad-key":1}}}}})");
  EXPECT_TRUE(!e.ok() && e.error().kind == ErrorKind::Validation);
  // The same names load in a v0.9 catalog, which predates the rules.
  e = load_error(R"({"catalogId":"x","protocolVersion":"v0.9","components":{"My-Comp":{}}})");
  EXPECT_TRUE(e.ok());
  // Allowed references: local, common_types.json (relative and absolute) and catalog.json.
  e = load_error(R"({"catalogId":"x","protocolVersion":"1.0","components":{"A":{"properties":{
      "a":{"$ref":"common_types.json#/$defs/DynamicString"},
      "b":{"$ref":"https://a2ui.org/specification/v1_0/common_types.json#/$defs/DynamicString"},
      "c":{"$ref":"#/$defs/X"},"d":{"$ref":"catalog.json#/$defs/X"}}}},"$defs":{"X":{}}})");
  EXPECT_TRUE(e.ok());
}

TEST(catalog_unions_filter_entries_and_round_trip) {
  const char* doc = R"({
    "catalogId": "x", "protocolVersion": "1.0",
    "components": {"Listed": {"type": "object"}, "Unlisted": {"type": "object"}},
    "functions": {"listed": {"returnType": "string"}, "unlisted": {"returnType": "string"}},
    "$defs": {
      "anyComponent": {"oneOf": [{"$ref": "#/components/Listed"}], "discriminator": {"propertyName": "component"}},
      "anyFunction": {"oneOf": [{"$ref": "#/functions/listed"}]}
    }
  })";
  auto r = Catalog::from_json(J(doc));
  EXPECT_TRUE(r.ok());
  if (!r.ok()) return;
  EXPECT_TRUE(r.value()->find_component("Listed") && !r.value()->find_component("Unlisted"));
  EXPECT_TRUE(r.value()->find_function("listed") && !r.value()->find_function("unlisted"));
  EXPECT_TRUE(r.value()->to_json() == J(doc));
}

TEST(catalog_derive) {
  CatalogPtr c = small();
  std::vector<a2ui::DeriveEntry> comps = {{"Label", Json()},
                                          {"Box", J(R"({"type":"object","properties":{"w":{"type":"number"}}})")}};
  std::vector<a2ui::DeriveEntry> fns = {{"upper", Json()}};
  const std::string instructions = "Use Box.";
  auto d = c->derive("test:derived", &comps, &fns, &instructions);
  EXPECT_TRUE(d.ok());
  if (!d.ok()) return;
  EXPECT_EQ(d.value()->id(), std::string("test:derived"));
  EXPECT_EQ(d.value()->components().size(), size_t(2));
  EXPECT_EQ(d.value()->functions().size(), size_t(1));
  const Json out = d.value()->to_json();
  EXPECT_EQ(out["catalogId"].as_string(), std::string("test:derived"));
  EXPECT_EQ(out["instructions"].as_string(), instructions);
  EXPECT_TRUE(out["components"].contains("Box") && !out["functions"].contains("secret"));
  std::vector<a2ui::DeriveEntry> missing = {{"Nope", Json()}};
  auto bad = c->derive("test:derived", &missing, nullptr);
  EXPECT_TRUE(!bad.ok() && bad.error().kind == ErrorKind::Catalog &&
              bad.error().message.find("Nope") != std::string::npos);
}

TEST(catalog_validation_schema_is_self_contained) {
  const Json s = basic()->validation_schema();
  EXPECT_TRUE(s["$defs"].contains("DynamicString"));
  EXPECT_TRUE(s["$defs"].contains("FunctionCall"));
  EXPECT_TRUE(!s["$defs"].contains("Checkable"));  // Flattened into the components.
  EXPECT_TRUE(s["components"]["Button"]["properties"].contains("@checks"));
  EXPECT_TRUE(s["components"]["Button"]["properties"].contains("id"));
  EXPECT_EQ(s["components"]["Text"]["properties"]["text"]["$ref"].as_string(),
            std::string("#/$defs/DynamicString"));
  // The bundle validates on its own, with no other document registered.
  a2ui::SchemaRegistry reg;
  auto self = std::make_shared<const Json>(s);
  reg.add("bundle.json", self);
  EXPECT_TRUE(a2ui::validate_ref(reg, "bundle.json#/components/Text",
                                 J(R"({"id":"t","component":"Text","text":{"@path":"/x"}})")).ok());
}

TEST(validate_component_envelope_and_properties) {
  const Catalog& c = *basic();
  EXPECT_TRUE(component_ok(c, R"({"id":"t","component":"Text","text":"hi"})"));
  EXPECT_TRUE(component_ok(c, R"({"id":"t","component":"Text","text":{"@path":"/a"},
      "@accessibility":{"label":"L"},"@metadata":{"extensions":{"x":1}}})"));
  EXPECT_TRUE(!component_ok(c, R"({"component":"Text","text":"hi"})"));               // No id.
  EXPECT_TRUE(!component_ok(c, R"({"id":"t","component":"Text"})"));                  // No text.
  EXPECT_TRUE(!component_ok(c, R"({"id":"t","component":"Text","text":"hi","x":1})"));  // Unknown.
  EXPECT_TRUE(!component_ok(c, R"({"id":"t","component":"Text","text":"hi","@catalogId":5})"));
  EXPECT_TRUE(!component_ok(c, R"({"id":"t","component":"Text","text":"hi","@checks":[]})"));  // Not Checkable.
  a2ui::ComponentValidationOptions relaxed;
  relaxed.allow_unknown_properties = true;
  EXPECT_TRUE(component_ok(c, R"({"id":"t","component":"Text","text":"hi","x":1})", relaxed));
  // Reserved single-@ keys in nested data must be escaped.
  EXPECT_TRUE(!component_ok(c, R"({"id":"b","component":"Button","child":"c",
      "action":{"event":{"name":"go","context":{"v":{"@meta":1}}}}})"));
  EXPECT_TRUE(component_ok(c, R"({"id":"b","component":"Button","child":"c",
      "action":{"event":{"name":"go","context":{"v":{"@@meta":1}}}}})"));
  // Issues carry JSON Pointer paths into the component.
  std::vector<a2ui::ValidationIssue> issues;
  const Json bad = J(R"({"id":"s","component":"Slider","value":"high","max":10})");
  auto st = a2ui::validate_component_properties(c, *c.find_component("Slider"), bad, &issues);
  EXPECT_TRUE(!st.ok() && st.error().kind == ErrorKind::Validation && st.error().code == "VALIDATION_FAILED");
  EXPECT_TRUE(!issues.empty() && issues[0].instance_path == "/value");
  EXPECT_TRUE(st.error().details.is_array() && !st.error().details.empty());
}

TEST(validate_component_legacy_bare_envelope_keys) {
  const Catalog& c = *small();
  // `catalogId` is declared by Label itself: an ordinary property.
  EXPECT_TRUE(component_ok(c, R"({"id":"l","component":"Label","text":"x","catalogId":"anything"})"));
  // `metadata` is not declared: read as @metadata during the transition.
  EXPECT_TRUE(component_ok(c, R"({"id":"l","component":"Label","text":"x","metadata":{"extensions":{"k":1}}})"));
  EXPECT_TRUE(!component_ok(c, R"({"id":"l","component":"Label","text":"x","metadata":{"bogus":1}})"));
  EXPECT_TRUE(!component_ok(c, R"({"id":"l","component":"Label","text":"x","metadata":{},"@metadata":{}})"));
  EXPECT_TRUE(!component_ok(c, R"({"id":"l","component":"Label","text":"x","size":0})"));  // Leaf $defs.
}

TEST(validate_component_nested_calls) {
  const Catalog& c = *small();
  // Runs here: fully checked.
  EXPECT_TRUE(component_ok(c, R"({"id":"l","component":"Label","text":{"@call":"upper","args":{"value":"a"}}})"));
  EXPECT_TRUE(!component_ok(c, R"({"id":"l","component":"Label","text":{"@call":"upper","args":{}}})"));
  EXPECT_TRUE(!component_ok(c, R"({"id":"l","component":"Label","text":{"@call":"upper","args":{"value":"a","x":1}}})"));
  EXPECT_TRUE(!component_ok(c, R"({"id":"l","component":"Label","text":{"@call":"nope"}})"));
  EXPECT_TRUE(!component_ok(c, R"({"id":"l","component":"Label","text":{"@call":"secret"}})"));  // agentOnly.
  // Nested inside args.
  EXPECT_TRUE(!component_ok(c, R"({"id":"l","component":"Label",
      "text":{"@call":"upper","args":{"value":{"@call":"nope"}}}})"));
  // Names another catalog: identifiers only.
  EXPECT_TRUE(component_ok(c, R"({"id":"l","component":"Label","text":{"@call":"nope","catalogId":"other","args":{"a":1}}})"));
  EXPECT_TRUE(!component_ok(c, R"({"id":"l","component":"Label","text":{"@call":"no-pe","catalogId":"other"}})"));
  EXPECT_TRUE(!component_ok(c, R"({"id":"l","component":"Label","text":{"@call":"f","catalogId":"other","args":{"a-b":1}}})"));
  // Names this catalog explicitly: fully checked.
  EXPECT_TRUE(!component_ok(c, R"({"id":"l","component":"Label","text":{"@call":"nope","catalogId":"test:small"}})"));
  // A component in another catalog than the default: calls naming no
  // catalog run in the default, so only identifiers are checked here.
  EXPECT_TRUE(component_ok(c, R"({"id":"l","component":"Label","@catalogId":"test:small","text":{"@call":"nope"}})"));
  a2ui::ComponentValidationOptions in_default;
  in_default.catalog_is_default = true;
  EXPECT_TRUE(!component_ok(c, R"({"id":"l","component":"Label","@catalogId":"test:small","text":{"@call":"nope"}})",
                            in_default));
  a2ui::ComponentValidationOptions unknown_ok;
  unknown_ok.allow_unknown_functions = true;
  EXPECT_TRUE(component_ok(c, R"({"id":"l","component":"Label","text":{"@call":"nope"}})", unknown_ok));
  // System functions.
  EXPECT_TRUE(component_ok(c, R"({"id":"l","component":"Label","text":{"@call":"@index","args":{"offset":1}}})"));
  EXPECT_TRUE(!component_ok(c, R"({"id":"l","component":"Label","text":{"@call":"@index","args":{"step":1}}})"));
  EXPECT_TRUE(!component_ok(c, R"({"id":"l","component":"Label","text":{"@call":"@index","catalogId":"test:small"}})"));
  EXPECT_TRUE(!component_ok(c, R"({"id":"l","component":"Label","text":{"@call":"@foo"}})"));
  // Checks and actions hold calls too.
  EXPECT_TRUE(!component_ok(c, R"({"id":"l","component":"Label","text":"x",
      "@checks":[{"condition":{"@call":"nope"}}]})"));
  EXPECT_TRUE(!component_ok(c, R"({"id":"l","component":"Label","text":"x",
      "onTap":{"functionCall":{"@call":"upper","args":{}}}})"));
}

TEST(validate_function_call_entry_point) {
  const Catalog& c = *small();
  EXPECT_TRUE(a2ui::validate_function_call(c, J(R"({"@call":"upper","args":{"value":"a"}})")).ok());
  EXPECT_TRUE(!a2ui::validate_function_call(c, J(R"({"@call":"upper","args":{"value":1}})")).ok());
  EXPECT_TRUE(!a2ui::validate_function_call(c, J(R"({"@call":"upper","args":{"value":"a"},"x":1})")).ok());
  EXPECT_TRUE(!a2ui::validate_function_call(c, J(R"({"args":{}})")).ok());
  EXPECT_TRUE(!a2ui::validate_function_call(c, J(R"({"@call":"upper","args":{"value":{"@call":"nope"}}})")).ok());
}

TEST(validate_common_types) {
  auto ok = [](const char* def, const char* v) { return a2ui::validate_common_type("1.0", def, J(v)).ok(); };
  EXPECT_TRUE(ok("DynamicString", R"("x")"));
  EXPECT_TRUE(ok("DynamicString", R"({"@path":"/a"})"));
  EXPECT_TRUE(!ok("DynamicString", "1"));
  EXPECT_TRUE(ok("DynamicValue", R"({"label":"x"})"));
  EXPECT_TRUE(!ok("DynamicValue", R"({"@path":"/a","extra":1})"));
  EXPECT_TRUE(ok("FunctionCall", R"({"@call":"x","catalogId":"c","args":{"a":1}})"));
  EXPECT_TRUE(!ok("FunctionCall", R"({"@call":"x","bogus":1})"));
  EXPECT_TRUE(ok("ChildList", R"({"componentId":"row","path":"/items"})"));
  EXPECT_TRUE(!ok("ChildList", R"({"componentId":"row"})"));
  EXPECT_TRUE(!ok("Surface", R"({"child":"main"})"));
  EXPECT_TRUE(!a2ui::validate_common_type("0.9", "DynamicString", J(R"("x")")).ok());
  EXPECT_TRUE(!a2ui::validate_common_type("1.0", "NoSuchType", J(R"("x")")).ok());
}

TEST(catalog_schema_registry_reaches_entries) {
  const Catalog& c = *small();
  a2ui::SchemaRegistry reg = c.schema_registry();
  EXPECT_TRUE(a2ui::validate_ref(reg, "catalog.json#/$defs/Size", J("2")).ok());
  EXPECT_TRUE(!a2ui::validate_ref(reg, "catalog.json#/$defs/Size", J("0")).ok());
  EXPECT_TRUE(a2ui::validate_ref(reg, "catalog.json#/functions/upper/properties/args",
                                 J(R"({"value":"a"})")).ok());
  EXPECT_TRUE(a2ui::validate_ref(reg, "catalog.json#/components/Label/properties/size", J("3")).ok());
}

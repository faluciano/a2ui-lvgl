// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for packed documents (a2ui/schema_node.h) and the compact
// catalog (Catalog::from_packed). The conformance suites also run with
// every catalog packed (`conformance --compact`).

#include <string>
#include <vector>

#include "a2ui/basic_catalog.h"
#include "a2ui/catalog.h"
#include "a2ui/message_processor.h"
#include "a2ui/schema.h"
#include "a2ui/schema_node.h"
#include "a2ui/spec_documents.h"
#include "a2ui/validation.h"
#include "test_main.h"

using a2ui::Catalog;
using a2ui::CatalogPtr;
using a2ui::Json;
using a2ui::PackedBuffer;
using a2ui::SchemaNode;
using a2ui::spec::Document;

namespace {

Json J(const char* text) {
  auto r = Json::parse(text);
  if (!r.ok) unit::report_failure(__FILE__, __LINE__, std::string("bad test JSON: ") + text);
  return r.value;
}

// Member order matters for a packed document (it is the document's), so
// compare dumps rather than with Json's order-blind operator==.
bool same_text(const Json& a, const Json& b) { return a.dump() == b.dump(); }

CatalogPtr json_basic() {
  static CatalogPtr c = Catalog::from_json(*a2ui::spec::parsed(Document::BasicCatalogV1)).value();
  return c;
}

CatalogPtr compact_basic() {
  static CatalogPtr c = Catalog::from_packed(*a2ui::spec::packed(Document::BasicCatalogV1)).value();
  return c;
}

}  // namespace

TEST(packed_spec_documents_match_the_text) {
  for (auto d : {Document::CommonTypesV1_0, Document::AgentToRendererV1_0, Document::BasicCatalogV1}) {
    const SchemaNode root = SchemaNode::root(*a2ui::spec::packed(d));
    EXPECT_TRUE(same_text(root.to_json(), *a2ui::spec::parsed(d)));
    EXPECT_TRUE(root.equals(*a2ui::spec::parsed(d)));
  }
  EXPECT_TRUE(a2ui::spec::packed_common_types("1.0") == a2ui::spec::packed(Document::CommonTypesV1_0));
  EXPECT_TRUE(a2ui::spec::packed_common_types("0.9") == nullptr);
}

TEST(packed_buffer_round_trips) {
  const Json doc = J(R"({"a": [1, -0.5, 1e300, true, false, null, "x", "é\u0000\"\\"],
                         "b": {}, "c": [], "d": {"a": "x", "z": [[[]]]}, "": 0})");
  PackedBuffer packed(doc);
  const SchemaNode root = SchemaNode::root(packed.document());
  EXPECT_TRUE(same_text(root.to_json(), doc));
  EXPECT_TRUE(root.equals(doc));
  EXPECT_TRUE(root.equals(SchemaNode(doc)));
  EXPECT_EQ(root.dump(), doc.dump());
  EXPECT_TRUE(root["a"].at(7).as_string() == std::string_view("é\0\"\\", 5));
  EXPECT_TRUE(root["d"]["z"].at(0).at(0).is_array());
  EXPECT_TRUE(!root["missing"].present() && !root["a"].at(99).present());
  EXPECT_TRUE(root[""].is_number());
  EXPECT_TRUE(!root.equals(J(R"({"a": 1})")));
  // Strings are stored once.
  PackedBuffer twice(J(R"(["abcdefgh", "abcdefgh", {"abcdefgh": "abcdefgh"}])"));
  EXPECT_TRUE(twice.byte_size() < 8 * 5 + 2 * 8 + 9);
}

TEST(packed_pointers_resolve_like_json) {
  const Json doc = J(R"({"$defs": {"a/b": {"x": [10, 20]}, "m~n": 1, "p q": 2}})");
  PackedBuffer packed(doc);
  const SchemaNode root = SchemaNode::root(packed.document());
  for (const char* p : {"", "/$defs", "/$defs/a~1b/x/1", "/$defs/m~0n", "/$defs/p%20q", "/$defs/a~1b/x/01",
                        "/$defs/a~1b/x/2", "/nope", "x"}) {
    const Json* want = a2ui::resolve_json_pointer(doc, p);
    const SchemaNode got = a2ui::resolve_node_pointer(root, p);
    EXPECT_EQ(want != nullptr, got.present());
    if (want && got) EXPECT_TRUE(got.equals(*want));
  }
}

TEST(compact_catalog_summaries_match_from_json) {
  const Catalog& a = *json_basic();
  const Catalog& b = *compact_basic();
  EXPECT_TRUE(b.is_compact() && !a.is_compact());
  EXPECT_EQ(a.id(), b.id());
  EXPECT_EQ(a.protocol_version(), b.protocol_version());
  EXPECT_EQ(a.declared_protocol_version(), b.declared_protocol_version());
  EXPECT_EQ(a.components().size(), b.components().size());
  EXPECT_EQ(a.functions().size(), b.functions().size());
  for (size_t i = 0; i < a.components().size() && i < b.components().size(); ++i) {
    const auto& x = a.components()[i];
    const auto& y = b.components()[i];
    EXPECT_EQ(x.name, y.name);
    EXPECT_TRUE(x.property_kinds == y.property_kinds);
    EXPECT_TRUE(x.nested_kinds == y.nested_kinds);
    EXPECT_TRUE(x.required == y.required);
    EXPECT_TRUE(x.allowed_parents == y.allowed_parents);
    EXPECT_TRUE(x.allowed_children == y.allowed_children);
    EXPECT_TRUE(x.metadata == y.metadata);
    EXPECT_TRUE(x.child_refs.single == y.child_refs.single);
    EXPECT_TRUE(x.child_refs.list == y.child_refs.list);
    EXPECT_TRUE(x.child_refs.nested == y.child_refs.nested);
    EXPECT_TRUE(x.custom_envelope_properties == y.custom_envelope_properties);
    EXPECT_EQ(x.checkable, y.checkable);
    EXPECT_TRUE(y.schema.is_undefined() && y.packed_schema.is_packed());
    EXPECT_TRUE(y.schema_node().equals(x.schema));
  }
  for (size_t i = 0; i < a.functions().size() && i < b.functions().size(); ++i) {
    const auto& x = a.functions()[i].api;
    const auto& y = b.functions()[i].api;
    EXPECT_EQ(x.name, y.name);
    EXPECT_EQ(x.return_type, y.return_type);
    EXPECT_TRUE(x.allowed_callers == y.allowed_callers);
    EXPECT_EQ(x.requires_user_activation, y.requires_user_activation);
    EXPECT_TRUE(y.schema_node().equals(x.schema));
    EXPECT_TRUE(y.args_schema_node().equals(x.args_schema));
  }
  EXPECT_TRUE(a.to_json() == b.to_json());
  EXPECT_TRUE(a.validation_schema() == b.validation_schema());
}

TEST(compact_catalog_validates_like_from_json) {
  const char* cases[] = {
      R"({"id": "t", "component": "Text", "text": "hi"})",
      R"({"id": "t", "component": "Text", "text": {"@path": "/x"}, "variant": "h1"})",
      R"({"id": "t", "component": "Text", "text": 5})",
      R"({"id": "t", "component": "Text", "text": "hi", "variant": "huge"})",
      R"({"id": "t", "component": "Text", "text": "hi", "extra": 1})",
      R"({"id": "b", "component": "Button", "child": "t", "action": {"event": {"name": "go"}}})",
      R"({"id": "b", "component": "Button", "child": "t"})",
      R"({"id": "f", "component": "TextField", "label": "Name", "value": {"@path": "/n"},
          "@checks": [{"condition": {"@call": "required", "args": {"value": {"@path": "/n"}}},
                       "message": "needed"}]})",
      R"({"id": "f", "component": "TextField", "label": "Name",
          "@checks": [{"condition": {"@call": "length", "args": {"value": "x", "min": "two"}},
                       "message": "bad"}]})",
      R"({"id": "r", "component": "Row", "children": ["a", "b"], "justify": "center"})",
      R"({"id": "r", "component": "Row", "children": {"componentId": "a", "path": "/items"}})",
      R"({"id": "s", "component": "Slider", "value": 3, "min": 0, "max": "ten"})",
  };
  for (const char* text : cases) {
    const Json c = J(text);
    const std::string type = c["component"].as_string();
    std::vector<a2ui::ValidationIssue> ia, ib;
    const a2ui::Status sa = a2ui::validate_component_properties(*json_basic(), *json_basic()->find_component(type),
                                                                c, &ia);
    const a2ui::Status sb = a2ui::validate_component_properties(
        *compact_basic(), *compact_basic()->find_component(type), c, &ib);
    EXPECT_EQ(sa.ok(), sb.ok());
    if (!sa.ok() && !sb.ok()) EXPECT_EQ(sa.error().message, sb.error().message);
    EXPECT_EQ(ia.size(), ib.size());
  }
  const Json call = J(R"({"@call": "formatNumber", "args": {"value": 3, "decimals": "x"}})");
  const a2ui::Status fa = a2ui::validate_function_call(*json_basic(), call);
  const a2ui::Status fb = a2ui::validate_function_call(*compact_basic(), call);
  EXPECT_EQ(fa.ok(), fb.ok());
  if (!fa.ok() && !fb.ok()) EXPECT_EQ(fa.error().message, fb.error().message);
}

TEST(compact_catalog_runs_a_card) {
  auto catalog = Catalog::from_packed(*a2ui::spec::packed(Document::BasicCatalogV1)).value();
  a2ui::basic_catalog::bind_functions(*catalog);
  a2ui::MessageProcessor processor({catalog});
  const Json messages = J(R"([
    {"version": "1.0", "createSurface": {"surfaceId": "s", "catalogId": "https://a2ui.org/specification/v1_0/catalogs/basic/catalog.json"}},
    {"version": "1.0", "updateComponents": {"surfaceId": "s", "components": [
      {"id": "root", "component": "Column", "children": ["t", "n"]},
      {"id": "t", "component": "Text", "text": {"@call": "formatString", "args": {"value": "Hi ${/name}"}}},
      {"id": "n", "component": "TextField", "label": "Name", "value": {"@path": "/name"}}
    ]}},
    {"version": "1.0", "updateDataModel": {"surfaceId": "s", "path": "/name", "value": "Ada"}}
  ])");
  EXPECT_TRUE(processor.process_messages(messages).ok());
  EXPECT_TRUE(processor.model().get_surface("s") != nullptr);
  // An invalid component is rejected the same way.
  const Json bad = J(R"([{"version": "1.0", "updateComponents": {"surfaceId": "s", "components": [
      {"id": "t", "component": "Text", "text": 5}]}}])");
  EXPECT_TRUE(!processor.process_messages(bad).ok());
}

TEST(compact_catalog_from_runtime_buffer_and_errors) {
  const Json doc = J(R"({
    "catalogId": "test:c", "protocolVersion": "1.0",
    "components": {"Label": {"type": "object", "properties": {"text": {"$ref": "common_types.json#/$defs/DynamicString"}},
                              "required": ["text"]}},
    "functions": {"twice": {"type": "object", "properties": {"@call": {"const": "twice"},
                            "args": {"type": "object", "properties": {"n": {"type": "number", "default": 2}}}},
                            "returnType": "number"}}
  })");
  PackedBuffer packed(doc);
  auto r = Catalog::from_packed(packed.document());
  EXPECT_TRUE(r.ok());
  if (r.ok()) {
    EXPECT_TRUE(r.value()->find_component("Label")->property_kinds.at("text") == a2ui::PropertyKind::DynamicString);
    EXPECT_EQ(r.value()->find_function("twice")->api.return_type, std::string("number"));
    EXPECT_TRUE(r.value()->to_json() == doc);
    // A derived catalog keeps pointing into the same packed document.
    std::vector<a2ui::DeriveEntry> keep = {{"Label", Json()}};
    auto d = r.value()->derive("test:d", &keep, nullptr);
    EXPECT_TRUE(d.ok() && d.value()->find_component("Label")->schema_node().is_packed());
  }
  PackedBuffer bad(J(R"({"catalogId": "x", "protocolVersion": "1.0", "components": {"Surface": {}}})"));
  auto e = Catalog::from_packed(bad.document());
  EXPECT_TRUE(!e.ok() && e.error().kind == a2ui::ErrorKind::Catalog);
}

TEST(basic_catalog_load_both_forms) {
  auto compact = a2ui::basic_catalog::load(true);
  auto parsed = a2ui::basic_catalog::load(false);
  EXPECT_TRUE(compact.ok() && parsed.ok());
  if (!compact.ok() || !parsed.ok()) return;
  EXPECT_TRUE(compact.value()->is_compact());
  EXPECT_TRUE(!parsed.value()->is_compact());
  EXPECT_TRUE(compact.value()->find_function("formatNumber")->has_local_impl());
  EXPECT_TRUE(parsed.value()->find_function("formatNumber")->has_local_impl());
  EXPECT_TRUE(compact.value()->to_json() == parsed.value()->to_json());
  // The host build does not define A2UI_COMPACT_CATALOG, so load() parses.
  EXPECT_TRUE(!a2ui::basic_catalog::compact_by_default());
}

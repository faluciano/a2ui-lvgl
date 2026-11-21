// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// State models and MessageProcessor behaviour the conformance suites do not
// observe: notification counts, model identity across updates, the
// incremental graph bound, and outbound error envelopes.

#include <memory>
#include <string>
#include <vector>

#include "a2ui/basic_catalog.h"
#include "a2ui/catalog.h"
#include "a2ui/message_processor.h"
#include "a2ui/spec_documents.h"
#include "a2ui/surface_model.h"
#include "test_main.h"

using a2ui::Json;
using a2ui::MessageProcessor;

namespace {

a2ui::CatalogPtr basic() {
  static const a2ui::CatalogPtr c = [] {
    auto r = a2ui::Catalog::from_json(*a2ui::spec::parsed(a2ui::spec::Document::BasicCatalogV1));
    a2ui::CatalogPtr cat = r.value();
    a2ui::basic_catalog::bind_functions(*cat);
    return cat;
  }();
  return c;
}

Json parse(const char* text) { return Json::parse(text).value; }

std::string create_surface(const char* extra) {
  return std::string(R"({"version":"1.0","createSurface":{"surfaceId":"s","catalogId":")") +
         a2ui::basic_catalog::kCatalogIdV1 + "\"" + extra + "}}";
}

}  // namespace

TEST(create_surface_writes_one_data_model_operation_per_top_level_key) {
  MessageProcessor p({basic()});
  std::vector<std::string> writes;
  int root_notifications = 0;
  std::vector<a2ui::Subscription> subs;
  auto created = p.model().on_surface_created().subscribe([&](a2ui::SurfaceModel* const& s) {
    subs.push_back(s->data_model().on_changed().subscribe([&](const std::string& path) { writes.push_back(path); }));
    subs.push_back(s->data_model().subscribe("/", [&](const Json&) { ++root_notifications; }));
  });
  auto st = p.process_messages(parse(create_surface(R"(,"dataModel":{"a":1,"b":{"c":2},"d":[1,2]})").c_str()));
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(writes.size(), size_t{3});
  EXPECT_EQ(root_notifications, 3);
  if (writes.size() == 3) {
    EXPECT_EQ(writes[0], std::string("/a"));
    EXPECT_EQ(writes[1], std::string("/b"));
    EXPECT_EQ(writes[2], std::string("/d"));
  }
  EXPECT_TRUE(p.model().get_surface("s")->data_model().root() == parse(R"({"a":1,"b":{"c":2},"d":[1,2]})"));
}

TEST(create_surface_applies_data_model_before_components) {
  MessageProcessor p({basic()});
  std::vector<std::string> order;
  std::vector<a2ui::Subscription> subs;
  auto created = p.model().on_surface_created().subscribe([&](a2ui::SurfaceModel* const& s) {
    subs.push_back(s->data_model().on_changed().subscribe([&](const std::string&) { order.push_back("data"); }));
    subs.push_back(s->components_model().on_created().subscribe(
        [&](a2ui::ComponentModel* const&) { order.push_back("component"); }));
  });
  auto st = p.process_messages(parse(create_surface(
      R"(,"dataModel":{"name":"Ada"},"components":[{"id":"root","component":"Text","text":{"@path":"/name"}}])")
                                         .c_str()));
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(order.size(), size_t{2});
  if (order.size() == 2) {
    EXPECT_EQ(order[0], std::string("data"));
    EXPECT_EQ(order[1], std::string("component"));
  }
}

TEST(update_components_keeps_model_for_same_type_and_replaces_on_type_change) {
  MessageProcessor p({basic()});
  EXPECT_TRUE(p.process_messages(parse(create_surface(
                                           R"(,"components":[{"id":"root","component":"Text","text":"a"}])")
                                           .c_str()))
                  .ok());
  a2ui::SurfaceModel* s = p.model().get_surface("s");
  a2ui::ComponentModel* before = s->components_model().get("root");
  int updated = 0, metadata_changes = 0, created = 0, deleted = 0;
  auto u = before->on_updated().subscribe([&](const a2ui::ComponentModel* const&) { ++updated; });
  auto m = before->on_metadata_changed().subscribe([&](const Json&) { ++metadata_changes; });
  auto c = s->components_model().on_created().subscribe([&](a2ui::ComponentModel* const&) { ++created; });
  auto d = s->components_model().on_deleted().subscribe([&](const std::string&) { ++deleted; });

  // Same type: updated in place, once.
  EXPECT_TRUE(p.process_messages(parse(
                  R"({"version":"1.0","updateComponents":{"surfaceId":"s","components":[{"id":"root","component":"Text","text":"b"}]}})"))
                  .ok());
  EXPECT_TRUE(s->components_model().get("root") == before);
  EXPECT_EQ(updated, 1);
  EXPECT_EQ(before->properties()["text"].as_string(), std::string("b"));

  // Metadata only: onMetadataChanged, not onUpdated.
  EXPECT_TRUE(p.process_messages(parse(
                  R"({"version":"1.0","updateComponents":{"surfaceId":"s","components":[{"id":"root","component":"Text","text":"b","@metadata":{"extensions":{"x":1}}}]}})"))
                  .ok());
  EXPECT_EQ(updated, 1);
  EXPECT_EQ(metadata_changes, 1);

  // Type change: a fresh model.
  EXPECT_TRUE(p.process_messages(parse(
                  R"({"version":"1.0","updateComponents":{"surfaceId":"s","components":[{"id":"root","component":"Column","children":[]}]}})"))
                  .ok());
  EXPECT_EQ(deleted, 1);
  EXPECT_EQ(created, 1);
  EXPECT_EQ(s->components_model().get("root")->type(), std::string("Column"));
}

TEST(rejected_batch_leaves_surface_unchanged_and_emits_error_envelope) {
  std::vector<Json> outbound;
  a2ui::MessageProcessorOptions o;
  o.outbound_listener = [&](const Json& m) { outbound.push_back(m); };
  MessageProcessor p({basic()}, o);
  EXPECT_TRUE(p.process_messages(parse(create_surface(
                                           R"(,"components":[{"id":"root","component":"Column","children":["a"]},{"id":"a","component":"Text","text":"x"}])")
                                           .c_str()))
                  .ok());
  auto st = p.process_messages(parse(
      R"({"version":"1.0","updateComponents":{"surfaceId":"s","components":[{"id":"a","component":"Column","children":["b"]},{"id":"b","component":"Column","children":["a"]}]}})"));
  EXPECT_TRUE(!st.ok());
  EXPECT_TRUE(st.error().is(a2ui::ErrorKind::Recursion));
  EXPECT_EQ(p.model().get_surface("s")->components_model().get("a")->type(), std::string("Text"));
  EXPECT_TRUE(!p.model().get_surface("s")->components_model().has("b"));
  EXPECT_EQ(outbound.size(), size_t{1});
  if (!outbound.empty()) {
    EXPECT_EQ(outbound[0]["version"].as_string(), std::string("1.0"));
    EXPECT_EQ(outbound[0]["error"]["code"].as_string(), std::string("RECURSION_LIMIT_EXCEEDED"));
    EXPECT_EQ(outbound[0]["error"]["surfaceId"].as_string(), std::string("s"));
  }

  auto missing = p.process_messages(parse(R"({"version":"1.0","deleteSurface":{"surfaceId":"s"}})"));
  EXPECT_TRUE(missing.ok());
  auto unknown = p.process_messages(parse(R"({"version":"1.0","updateDataModel":{"surfaceId":"s","value":1}})"));
  EXPECT_TRUE(unknown.error().is(a2ui::ErrorKind::Integrity));
  EXPECT_EQ(unknown.error().code, std::string("INTEGRITY_ERROR"));
}

TEST(relaxed_config_checks_batches_incrementally_and_still_finds_cycles) {
  a2ui::MessageProcessorOptions o;
  o.validation_config = a2ui::ValidationConfig::relaxed();
  MessageProcessor p({basic()}, o);
  EXPECT_TRUE(p.process_messages(parse(create_surface(
                                           R"(,"components":[{"id":"root","component":"Column","children":["a"]}])")
                                           .c_str()))
                  .ok());
  // Each message references the next component before it arrives.
  for (int i = 0; i < 20; ++i) {
    const std::string id = i == 0 ? "a" : "n" + std::to_string(i);
    const std::string next = "n" + std::to_string(i + 1);
    const std::string msg = R"({"version":"1.0","updateComponents":{"surfaceId":"s","components":[{"id":")" + id +
                            R"(","component":"Column","children":[")" + next + R"("]}]}})";
    EXPECT_TRUE(p.process_messages(parse(msg.c_str())).ok());
  }
  // Closing the chain back to the root is a cycle.
  auto st = p.process_messages(parse(
      R"({"version":"1.0","updateComponents":{"surfaceId":"s","components":[{"id":"n20","component":"Column","children":["root"]}]}})"));
  EXPECT_TRUE(!st.ok() && st.error().is(a2ui::ErrorKind::Recursion));
  // A chain past the depth limit fails too.
  std::string deep = R"({"version":"1.0","updateComponents":{"surfaceId":"s","components":[)";
  for (int i = 20; i < 60; ++i) {
    if (i > 20) deep += ",";
    deep += R"({"id":"n)" + std::to_string(i) + R"(","component":"Column","children":["n)" + std::to_string(i + 1) +
            R"("]})";
  }
  deep += "]}}";
  auto too_deep = p.process_messages(parse(deep.c_str()));
  EXPECT_TRUE(!too_deep.ok() && too_deep.error().is(a2ui::ErrorKind::Recursion));
}

TEST(surface_metadata_holder_semantics) {
  a2ui::SurfaceModel s("s", basic(), {});
  int changes = 0, signal = 0;
  auto c = s.on_metadata_changed().subscribe([&](const Json&) { ++changes; });
  auto g = s.metadata_signal().subscribe([&](const Json&) { ++signal; });
  EXPECT_TRUE(s.set_extension("trace", Json("t1")).ok());
  EXPECT_TRUE(s.set_extension("trace", Json("t1")).ok());  // Equal: no-op.
  EXPECT_EQ(changes, 1);
  EXPECT_EQ(signal, 1);
  EXPECT_TRUE(!s.set_extension("bad-key", Json(1)).ok());
  EXPECT_TRUE(s.set_extension("nothing", Json(nullptr)).ok());
  EXPECT_TRUE(s.extensions()["nothing"].is_null());
  s.remove_extension("trace");
  s.remove_extension("nothing");
  EXPECT_TRUE(s.metadata().is_undefined());
  EXPECT_EQ(changes, 4);
  EXPECT_TRUE(!s.set_metadata(parse(R"({"owner":"me"})")).ok());
}

TEST(dispatch_action_builds_renderer_action) {
  a2ui::SurfaceGroupModel group;
  auto owned = std::make_unique<a2ui::SurfaceModel>("s", basic(), a2ui::CatalogMap{});
  a2ui::SurfaceModel* s = owned.get();
  s->set_clock([] { return 1.5; });
  EXPECT_TRUE(group.add_surface(std::move(owned)).ok());
  EXPECT_TRUE(!group.add_surface(std::make_unique<a2ui::SurfaceModel>("s", basic(), a2ui::CatalogMap{})).ok());
  std::vector<Json> actions;
  auto sub = group.on_action().subscribe([&](const Json& a) { actions.push_back(a); });
  EXPECT_TRUE(s->data_model().set("/user", Json("Ada")).ok());
  Json resolved = s->resolve_action(parse(R"({"event":{"name":"go","context":{"who":{"@path":"/user"}}}})"));
  EXPECT_TRUE(s->dispatch_action(resolved, "btn", parse(R"({"extensions":{"k":1}})")).ok());
  EXPECT_EQ(actions.size(), size_t{1});
  if (!actions.empty()) {
    EXPECT_TRUE(actions[0] == parse(R"({"name":"go","surfaceId":"s","sourceComponentId":"btn",
      "timestamp":"1970-01-01T00:00:01.500Z","context":{"who":"Ada"},"metadata":{"extensions":{"k":1}}})"));
  }
  std::vector<std::string> deleted;
  auto d = group.on_surface_deleted().subscribe([&](const std::string& id) { deleted.push_back(id); });
  group.delete_surface("s");
  EXPECT_EQ(deleted.size(), size_t{1});
  EXPECT_TRUE(group.get_surface("s") == nullptr);
}

TEST(child_ids_follow_the_catalog_reference_map) {
  MessageProcessor p({basic()});
  EXPECT_TRUE(p.process_messages(parse(create_surface(R"(,"components":[
      {"id":"root","component":"Tabs","tabs":[{"title":"A","child":"a"},{"title":"B","child":"b"}]},
      {"id":"a","component":"Text","text":"a"},{"id":"b","component":"Card","child":"c"},
      {"id":"c","component":"List","children":{"componentId":"t","path":"/items"}},
      {"id":"t","component":"Text","text":"t"}])")
                                           .c_str()))
                  .ok());
  auto& m = p.model().get_surface("s")->components_model();
  EXPECT_TRUE(m.get_child_ids("root") == std::vector<std::string>({"a", "b"}));
  EXPECT_TRUE(m.get_child_ids("b") == std::vector<std::string>({"c"}));
  EXPECT_TRUE(m.get_child_ids("c") == std::vector<std::string>({"t"}));
  EXPECT_TRUE(m.get_child_ids("a").empty());
}

TEST(processor_accepts_wrapper_and_text_and_reports_capabilities) {
  MessageProcessor p({basic()});
  EXPECT_TRUE(p.process_text(R"({"messages":[{"version":"1.0","createSurface":{"surfaceId":"x","sendDataModel":true}}]})").ok());
  EXPECT_TRUE(p.model().get_surface("x") != nullptr);
  auto bad = p.process_text("{nope");
  EXPECT_TRUE(!bad.ok() && bad.error().kind == a2ui::ErrorKind::Parse);
  auto caps = p.get_renderer_capabilities({{"1.0"}, false, ""});
  EXPECT_TRUE(caps.ok());
  EXPECT_TRUE(caps.value() == Json::object({{"1.0", Json::object({{"supportedCatalogIds",
                                                                   Json::array({Json(a2ui::basic_catalog::kCatalogIdV1)})}})}}));
  auto none = p.get_renderer_capabilities({});
  EXPECT_TRUE(!none.ok());
  auto dm = p.get_renderer_data_model();
  EXPECT_TRUE(dm.ok() && dm.value() == parse(R"({"version":"1.0","surfaces":{"x":{}}})"));
  p.dispose();
  EXPECT_TRUE(p.model().surfaces().empty());
  EXPECT_TRUE(!p.process_text("[]").ok());
}

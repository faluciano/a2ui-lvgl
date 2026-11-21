// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// NodeResolver behaviour beyond the node_resolution conformance suite:
// node identity across data and definition changes, notification counts,
// subscription cleanup (leak checks) and the adapter-facing helpers.

#include <memory>
#include <string>
#include <vector>

#include "a2ui/basic_catalog.h"
#include "a2ui/catalog.h"
#include "a2ui/message_processor.h"
#include "a2ui/node_resolver.h"
#include "a2ui/spec_documents.h"
#include "a2ui/surface_model.h"
#include "test_main.h"

using a2ui::ComponentNode;
using a2ui::Json;
using a2ui::MessageProcessor;
using a2ui::NodePtr;
using a2ui::NodeResolver;

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

Json parse(const std::string& text) {
  auto r = Json::parse(text);
  if (!r.ok) std::printf("bad test JSON: %s\n", r.error.c_str());
  return r.value;
}

a2ui::MessageProcessorOptions streaming() {
  a2ui::MessageProcessorOptions o;
  o.validation_config = a2ui::ValidationConfig::relaxed();  // Partial trees, as while streaming.
  return o;
}

/** A processor with surface "s" and a resolver attached to it. */
struct Fixture {
  MessageProcessor processor{{basic()}, streaming()};
  std::unique_ptr<NodeResolver> resolver;
  a2ui::Subscription created;
  std::vector<Json> errors;
  std::vector<Json> actions;
  std::vector<a2ui::Subscription> subs;

  explicit Fixture(const std::string& data_model = "{}") {
    created = processor.model().on_surface_created().subscribe([this](a2ui::SurfaceModel* const& s) {
      resolver = std::make_unique<NodeResolver>(*s);
      subs.push_back(s->on_error().subscribe([this](const Json& e) { errors.push_back(e); }));
      subs.push_back(s->on_action().subscribe([this](const Json& a) { actions.push_back(a); }));
    });
    EXPECT_TRUE(send(R"({"version":"1.0","createSurface":{"surfaceId":"s","catalogId":")" +
                     std::string(a2ui::basic_catalog::kCatalogIdV1) + R"(","dataModel":)" + data_model + "}}"));
  }

  a2ui::SurfaceModel& surface() { return *processor.model().get_surface("s"); }

  bool send(const std::string& message) {
    auto st = processor.process_messages(parse(message));
    if (!st.ok()) std::printf("process failed: %s\n", st.error().message.c_str());
    return st.ok();
  }

  bool components(const std::string& list) {
    return send(R"({"version":"1.0","updateComponents":{"surfaceId":"s","components":)" + list + "}}");
  }

  bool data(const std::string& path, const std::string& value) {
    return send(R"({"version":"1.0","updateDataModel":{"surfaceId":"s","path":")" + path + R"(","value":)" + value +
                "}}");
  }
};

std::vector<ComponentNode*> children_of(const ComponentNode* n) {
  std::vector<ComponentNode*> out;
  if (n) n->children(&out);
  return out;
}

/** Counts props emissions of one node. */
struct Counter {
  int props = 0;
  int destroyed = 0;
  a2ui::Subscription a, b;
  explicit Counter(ComponentNode* n) {
    a = n->on_props_changed([this](const a2ui::NodeProps&) { ++props; });
    b = n->on_destroyed().subscribe([this]() { ++destroyed; });
  }
};

const char* kList =
    R"([{"id":"root","component":"Column","children":{"componentId":"row","path":"/items"}},)"
    R"({"id":"row","component":"Text","text":{"@path":"name"}}])";

}  // namespace

TEST(template_items_keep_identity_by_index_and_grow_and_shrink) {
  Fixture f(R"({"items":[{"name":"a"},{"name":"b"}]})");
  EXPECT_TRUE(f.components(kList));
  ComponentNode* root = f.resolver->root();
  EXPECT_TRUE(root != nullptr);
  auto kids = children_of(root);
  EXPECT_EQ(kids.size(), size_t{2});
  if (kids.size() != 2) return;
  EXPECT_EQ(kids[0]->data_path(), std::string("/items/0"));
  EXPECT_EQ(kids[0]->index(), std::optional<long>(0));
  EXPECT_EQ(kids[1]->instance_id(), std::string("row-[/items/1]"));
  Counter root_c(root), k0(kids[0]), k1(kids[1]);

  // Insert at the front: index-keyed items keep their nodes and take new values.
  EXPECT_TRUE(f.data("/items", R"([{"name":"z"},{"name":"a"},{"name":"b"}])"));
  auto after = children_of(root);
  EXPECT_EQ(after.size(), size_t{3});
  EXPECT_TRUE(after[0] == kids[0] && after[1] == kids[1]);
  EXPECT_EQ(root_c.props, 1);
  EXPECT_EQ(k0.props, 1);
  EXPECT_EQ(k1.props, 1);
  EXPECT_EQ(after[0]->prop("text")->value(), Json("z"));

  // Shrink: the dropped item's node is disposed once.
  Counter k2(after[2]);
  EXPECT_TRUE(f.data("/items", R"([{"name":"z"}])"));
  EXPECT_EQ(children_of(root).size(), size_t{1});
  EXPECT_EQ(k2.destroyed, 1);
  EXPECT_EQ(k1.destroyed, 1);
  EXPECT_EQ(k0.destroyed, 0);
  EXPECT_EQ(f.resolver->node_count(), size_t{2});
}

TEST(explicit_list_reorder_moves_nodes_without_rebuilding_them) {
  Fixture f;
  EXPECT_TRUE(f.components(R"([{"id":"root","component":"Column","children":["a","b"]},)"
                           R"({"id":"a","component":"Text","text":"A"},{"id":"b","component":"Text","text":"B"}])"));
  auto kids = children_of(f.resolver->root());
  EXPECT_EQ(kids.size(), size_t{2});
  if (kids.size() != 2) return;
  Counter root_c(f.resolver->root()), a(kids[0]), b(kids[1]);
  int children_changed = 0;
  auto sub = f.resolver->on_children_changed().subscribe([&](ComponentNode* const& n) {
    if (n == f.resolver->root()) ++children_changed;
  });
  EXPECT_TRUE(f.components(R"([{"id":"root","component":"Column","children":["b","a"]}])"));
  auto after = children_of(f.resolver->root());
  EXPECT_TRUE(after.size() == 2 && after[0] == kids[1] && after[1] == kids[0]);
  EXPECT_EQ(root_c.props, 1);
  EXPECT_EQ(a.props + b.props, 0);
  EXPECT_EQ(a.destroyed + b.destroyed, 0);
  EXPECT_EQ(children_changed, 1);
}

TEST(placeholder_is_replaced_in_place_and_type_change_replaces_node) {
  Fixture f;
  EXPECT_TRUE(f.components(R"([{"id":"root","component":"Card","child":"x"}])"));
  ComponentNode* root = f.resolver->root();
  EXPECT_TRUE(root != nullptr);
  if (!root) return;
  ComponentNode* pending = root->prop("child")->node();
  EXPECT_TRUE(pending && pending->state() == a2ui::NodeState::Pending);
  EXPECT_EQ(pending->type(), std::string("Placeholder"));
  const std::string instance = pending->instance_id();
  NodePtr keep = pending->shared_from_this();
  Counter root_c(root);

  EXPECT_TRUE(f.components(R"([{"id":"x","component":"Text","text":"hi"}])"));
  ComponentNode* text = root->prop("child")->node();
  EXPECT_TRUE(text != pending && text->state() == a2ui::NodeState::Resolved);
  EXPECT_EQ(text->instance_id(), instance);
  EXPECT_TRUE(keep->disposed());
  EXPECT_EQ(root_c.props, 1);

  // Same type: same node. Different type: a new node under the same instance id.
  Counter text_c(text);
  EXPECT_TRUE(f.components(R"([{"id":"x","component":"Text","text":"hello"}])"));
  EXPECT_TRUE(root->prop("child")->node() == text);
  EXPECT_EQ(text_c.props, 1);
  EXPECT_TRUE(f.components(R"([{"id":"x","component":"Divider"}])"));
  ComponentNode* divider = root->prop("child")->node();
  EXPECT_TRUE(divider != text && divider->type() == "Divider");
  EXPECT_EQ(divider->instance_id(), instance);
  EXPECT_EQ(text_c.destroyed, 1);
  EXPECT_EQ(root_c.props, 2);
}

TEST(only_nodes_reading_a_path_re_resolve) {
  Fixture f(R"({"a":"1","b":"2"})");
  EXPECT_TRUE(f.components(R"([{"id":"root","component":"Column","children":["ta","tb"]},)"
                           R"({"id":"ta","component":"Text","text":{"@path":"/a"}},)"
                           R"({"id":"tb","component":"Text","text":{"@path":"/b"}}])"));
  auto kids = children_of(f.resolver->root());
  if (kids.size() != 2) return;
  Counter root_c(f.resolver->root()), ta(kids[0]), tb(kids[1]);
  EXPECT_TRUE(f.data("/a", R"("10")"));
  EXPECT_EQ(ta.props, 1);
  EXPECT_EQ(tb.props + root_c.props, 0);
  EXPECT_TRUE(f.data("/c", "1"));  // Nobody reads /c.
  EXPECT_EQ(ta.props + tb.props + root_c.props, 1);
  EXPECT_TRUE(f.data("/a", R"("10")"));  // Unchanged value.
  EXPECT_EQ(ta.props, 1);
}

TEST(batched_writes_emit_once_with_the_final_state) {
  Fixture f(R"({"a":"1","b":"2"})");
  EXPECT_TRUE(f.components(R"([{"id":"root","component":"TextField","label":{"@path":"/a"},"value":{"@path":"/b"}}])"));
  ComponentNode* root = f.resolver->root();
  Counter c(root);
  Json last_label;
  auto sub = root->on_props_changed([&](const a2ui::NodeProps& p) { last_label = p.find("label")->value(); });
  {
    a2ui::Batch batch;
    (void)f.surface().data_model().set("/a", Json("x"));
    (void)f.surface().data_model().set("/b", Json("y"));
    (void)f.surface().data_model().set("/a", Json("z"));
  }
  EXPECT_EQ(c.props, 1);
  EXPECT_EQ(last_label, Json("z"));
  EXPECT_EQ(root->prop("value")->value(), Json("y"));
}

TEST(bindings_write_at_their_scope_and_actions_dispatch_with_metadata) {
  Fixture f(R"({"items":[{"name":"a"},{"name":"b"}]})");
  EXPECT_TRUE(f.components(
      R"([{"id":"root","component":"Column","children":{"componentId":"item","path":"/items"}},)"
      R"({"id":"item","component":"Button","child":"label","action":{"event":{"name":"pick",)"
      R"("context":{"n":{"@path":"name"},"i":{"@call":"@index"}}}}},)"
      R"({"id":"label","component":"TextField","label":"L","value":{"@path":"name"}}])"));
  auto items = children_of(f.resolver->root());
  if (items.size() != 2) return;
  ComponentNode* field = items[1]->prop("child")->node();
  EXPECT_EQ(field->data_path(), std::string("/items/1"));
  EXPECT_TRUE(field->prop("value")->writable());
  EXPECT_TRUE(!field->prop("label")->writable());
  EXPECT_TRUE(field->write("value", Json("B")).ok());
  EXPECT_EQ(f.surface().data_model().value_at("/items/1/name"), Json("B"));
  EXPECT_TRUE(!field->write("label", Json("nope")).ok());

  EXPECT_TRUE(f.actions.empty());
  EXPECT_TRUE(items[1]->invoke("action", parse(R"({"extensions":{"com_example_x":"1"}})")).ok());
  EXPECT_EQ(f.actions.size(), size_t{1});
  if (f.actions.size() == 1) {
    EXPECT_EQ(f.actions[0]["context"], parse(R"({"n":"B","i":1})"));
    EXPECT_EQ(f.actions[0]["sourceComponentId"], Json("item"));
    EXPECT_EQ(f.actions[0]["metadata"], parse(R"({"extensions":{"com_example_x":"1"}})"));
  }
}

TEST(checks_publish_validity_reactively) {
  Fixture f(R"({"v":""})");
  EXPECT_TRUE(f.components(
      R"([{"id":"root","component":"TextField","label":"Name","value":{"@path":"/v"},)"
      R"("@checks":[{"condition":{"@call":"required","args":{"value":{"@path":"/v"}}},"message":"Needed"}]}])"));
  ComponentNode* root = f.resolver->root();
  EXPECT_TRUE(!root->is_valid());
  EXPECT_EQ(root->validation_errors(), parse(R"(["Needed"])"));
  Counter c(root);
  EXPECT_TRUE(root->write("value", Json("x")).ok());
  EXPECT_TRUE(root->is_valid());
  EXPECT_EQ(root->validation_errors(), parse("[]"));
  EXPECT_EQ(c.props, 1);
}

TEST(subscriptions_are_released_on_unmount_and_dispose) {
  Fixture f(R"({"items":[{"name":"a"},{"name":"b"},{"name":"c"}],"t":"x"})");
  EXPECT_TRUE(f.components(
      R"([{"id":"root","component":"Column","children":["list","title"]},)"
      R"({"id":"list","component":"Column","children":{"componentId":"row","path":"/items"}},)"
      R"({"id":"row","component":"Text","text":{"@path":"name"}},)"
      R"({"id":"title","component":"Text","text":{"@path":"/t"}}])"));
  NodeResolver& r = *f.resolver;
  a2ui::DataModel& dm = f.surface().data_model();
  EXPECT_EQ(r.node_count(), size_t{6});
  const size_t mounted_subs = r.active_subscription_count();
  // Data observers: /items (list), 3 rows, /t (title); nothing else observes.
  EXPECT_EQ(dm.observer_count(), size_t{5});

  // Unmount the list subtree.
  EXPECT_TRUE(f.components(R"([{"id":"root","component":"Column","children":["title"]}])"));
  EXPECT_EQ(r.node_count(), size_t{2});
  EXPECT_EQ(dm.observer_count(), size_t{1});
  EXPECT_TRUE(r.active_subscription_count() < mounted_subs);

  // Remount, then dispose: every subscription goes.
  EXPECT_TRUE(f.components(R"([{"id":"root","component":"Column","children":["list","title"]}])"));
  EXPECT_EQ(r.node_count(), size_t{6});
  EXPECT_EQ(r.active_subscription_count(), mounted_subs);
  r.dispose();
  EXPECT_EQ(r.node_count(), size_t{0});
  EXPECT_EQ(r.active_subscription_count(), size_t{0});
  EXPECT_EQ(dm.observer_count(), size_t{0});
  EXPECT_TRUE(r.root() == nullptr);
}

TEST(delete_surface_disposes_the_tree_and_releases_everything) {
  Fixture f(R"({"t":"x"})");
  EXPECT_TRUE(f.components(R"([{"id":"root","component":"Card","child":"t"},)"
                           R"({"id":"t","component":"Text","text":{"@path":"/t"}}])"));
  NodePtr root = f.resolver->root_node().value();
  NodePtr text = root->prop("child")->node_ptr();
  Counter rc(root.get()), tc(text.get());
  int cleanups = 0;
  text->add_cleanup([&] { ++cleanups; });
  EXPECT_TRUE(f.send(R"({"version":"1.0","deleteSurface":{"surfaceId":"s"}})"));
  EXPECT_TRUE(f.resolver->disposed());
  EXPECT_TRUE(root->disposed() && text->disposed());
  EXPECT_EQ(rc.destroyed + tc.destroyed, 2);
  EXPECT_EQ(cleanups, 1);
  EXPECT_EQ(f.resolver->active_subscription_count(), size_t{0});
  EXPECT_EQ(f.resolver->node_count(), size_t{0});
  EXPECT_TRUE(!text->write("text", Json("y")).ok());
}

TEST(cycles_and_the_depth_bound_resolve_to_placeholders) {
  Fixture f;
  EXPECT_TRUE(f.components(R"([{"id":"root","component":"Card","child":"a"},)"
                           R"({"id":"a","component":"Card","child":"b"},{"id":"b","component":"Card","child":"c"},)"
                           R"({"id":"c","component":"Card","child":"d"},{"id":"d","component":"Text","text":"x"}])"));
  EXPECT_EQ(f.resolver->node_count(), size_t{5});
  a2ui::NodeResolverOptions shallow;
  shallow.max_depth = 2;
  f.errors.clear();
  NodeResolver limited(f.surface(), shallow);
  ComponentNode* n = limited.root();
  for (int i = 0; i < 3 && n; ++i) n = n->prop("child")->node();
  EXPECT_TRUE(n && n->state() == a2ui::NodeState::Cyclic && n->component_id() == "c");
  EXPECT_EQ(limited.node_count(), size_t{4});
  EXPECT_EQ(f.errors.size(), size_t{1});
}

TEST(metadata_updates_do_not_touch_props) {
  Fixture f;
  EXPECT_TRUE(f.components(R"([{"id":"root","component":"Text","text":"a"}])"));
  ComponentNode* root = f.resolver->root();
  EXPECT_TRUE(root->metadata().is_undefined());
  Counter c(root);
  int meta = 0;
  auto sub = root->metadata_signal().subscribe([&](const Json&) { ++meta; });
  EXPECT_TRUE(f.components(R"([{"id":"root","component":"Text","text":"a","@metadata":{"extensions":{"com_x":"1"}}}])"));
  EXPECT_EQ(meta, 1);
  EXPECT_EQ(c.props, 0);
  EXPECT_EQ(root->metadata(), parse(R"({"extensions":{"com_x":"1"}})"));
}

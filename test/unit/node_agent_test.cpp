// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// NodeResolver + RPC integration: a binding that calls an agent function
// renders empty while the call is pending and re-resolves when it settles.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "a2ui/basic_catalog.h"
#include "a2ui/catalog.h"
#include "a2ui/message_processor.h"
#include "a2ui/node_resolver.h"
#include "a2ui/spec_documents.h"
#include "test_main.h"

using a2ui::Json;

namespace {

Json parse(const char* text) { return Json::parse(text).value; }

/** The basic catalog plus a schema-only `greeting` function the agent answers. */
a2ui::CatalogPtr catalog_with_agent_function() {
  auto r = a2ui::Catalog::from_json(*a2ui::spec::parsed(a2ui::spec::Document::BasicCatalogV1));
  auto cat = r.value();
  a2ui::basic_catalog::bind_functions(*cat);
  a2ui::FunctionDecl decl;
  decl.api.name = "greeting";
  decl.api.return_type = "string";
  decl.api.args_schema =
      parse(R"({"type":"object","properties":{"name":{"$ref":"common_types.json#/$defs/DynamicString"}}})");
  decl.api.allowed_callers = a2ui::AllowedCallers::RendererOrAgent;
  cat->add_function(std::move(decl));  // No impl: calls route to the agent.
  return cat;
}

std::string text_of(const a2ui::ComponentNode* n) {
  const a2ui::PropValue* t = n ? n->prop("text") : nullptr;
  return t && t->value().is_string() ? t->value().as_string() : std::string("<none>");
}

}  // namespace

TEST(agent_bound_text_re_resolves_when_the_call_settles) {
  std::vector<Json> outbound;
  a2ui::MessageProcessorOptions opts;
  opts.outbound_listener = [&outbound](const Json& m) { outbound.push_back(m); };
  a2ui::MessageProcessor processor({catalog_with_agent_function()}, std::move(opts));

  const std::string cat = a2ui::basic_catalog::kCatalogIdV1;
  Json create = parse(R"({"version":"1.0","createSurface":{"surfaceId":"s","components":[
      {"id":"root","component":"Text","text":{"@call":"greeting","args":{"name":{"@path":"/name"}}}}],
      "dataModel":{"name":"Ada"}}})");
  Json& cs = *create.find_mut("createSurface");
  cs.set("catalogId", cat);
  auto created = processor.process_messages(create);
  if (!created.ok()) std::printf("createSurface: %s\n", created.error().message.c_str());
  EXPECT_TRUE(created.ok());

  a2ui::SurfaceModel* surface = processor.model().get_surface("s");
  EXPECT_TRUE(surface != nullptr);
  if (!surface) return;
  a2ui::NodeResolver resolver(*surface);
  int emissions = 0;
  auto sub = resolver.root()->on_props_changed([&emissions](const a2ui::NodeProps&) { ++emissions; });

  // Pending: the call went out, the text is empty.
  std::string call_id;
  for (const auto& m : outbound) {
    if (m.contains("callAgentFunction")) call_id = m["callAgentFunction"]["functionCallId"].as_string();
  }
  EXPECT_TRUE(!call_id.empty());
  EXPECT_TRUE(text_of(resolver.root()) != "Hello, Ada");

  // The agent answers: the node re-resolves and shows the value.
  Json body = Json::object({{"functionCallId", Json(call_id)}, {"value", Json("Hello, Ada")}});
  EXPECT_TRUE(processor.process_messages(Json::object({{"version", Json("1.0")}, {"agentFunctionResponse", body}})).ok());
  EXPECT_EQ(text_of(resolver.root()), std::string("Hello, Ada"));
  EXPECT_EQ(emissions, 1);

  resolver.dispose();
  EXPECT_EQ(resolver.active_subscription_count(), size_t(0));
}

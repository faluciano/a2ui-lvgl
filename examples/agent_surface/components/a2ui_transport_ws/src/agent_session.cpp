// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0

#include "a2ui_transport_ws/agent_session.h"

#include <utility>
#include <vector>

namespace a2ui_transport_ws {

namespace {

// The data model of every surface with sendDataModel, or null.
a2ui::Json renderer_data_model(const a2ui::MessageProcessor& processor) {
  a2ui::Result<a2ui::Json> model = processor.get_renderer_data_model("1.0");
  if (!model.ok() || !model.value().is_object()) return a2ui::Json::null();
  return model.value();
}

}  // namespace

AgentSession::AgentSession(a2ui::MessageProcessor& processor, SendText send)
    : processor_(processor), send_(std::move(send)) {
  processor_.set_outbound_listener([this](const a2ui::Json& message) { send_message(message); });
}

AgentSession::~AgentSession() { processor_.set_outbound_listener(nullptr); }

a2ui::Json AgentSession::start_frame(const a2ui::Json& previous_data_model) const {
  a2ui::CapabilitiesOptions options;
  options.versions = {"1.0"};
  a2ui::Json metadata = a2ui::Json::object();
  a2ui::Result<a2ui::Json> caps = processor_.get_renderer_capabilities(options);
  if (caps.ok()) metadata.set("a2uiRendererCapabilities", caps.value());
  if (previous_data_model.is_object()) metadata.set("a2uiRendererDataModel", previous_data_model);
  return a2ui::Json::object({{"metadata", std::move(metadata)}});
}

void AgentSession::connected() {
  // What the user saw (and typed) last time, for the agent to restore.
  const a2ui::Json previous = renderer_data_model(processor_);

  // Calls from the previous connection can no longer be answered. disconnected()
  // normally did this already; a transport may report only the new connection.
  connected_ = false;
  processor_.rpc().cancel_all_calls();

  // The agent creates its surfaces again, and createSurface for an id that
  // still exists is an INTEGRITY_ERROR in v1.0. Views on these surfaces
  // unmount themselves.
  std::vector<std::string> ids;
  for (const auto& entry : processor_.model().surfaces()) ids.push_back(entry.first);
  for (const std::string& id : ids) processor_.model().delete_surface(id);

  connected_ = true;
  ++stats_.sessions;
  send_frame(start_frame(previous));
}

void AgentSession::disconnected() {
  connected_ = false;
  processor_.rpc().cancel_all_calls();
}

a2ui::Status AgentSession::receive(std::string_view frame) {
  ++stats_.frames_in;
  a2ui::Status status = processor_.process_text(frame);
  if (!status.ok()) ++stats_.frames_rejected;
  return status;
}

void AgentSession::send_message(const a2ui::Json& message) {
  if (!connected_) {
    ++stats_.dropped_out;
    return;
  }
  a2ui::Json frame = a2ui::Json::object({{"message", message}});
  a2ui::Json model = renderer_data_model(processor_);
  if (!model.is_null()) {
    frame.set("metadata", a2ui::Json::object({{"a2uiRendererDataModel", std::move(model)}}));
  }
  send_frame(frame);
}

bool AgentSession::send_frame(const a2ui::Json& frame) {
  if (send_ && send_(frame.dump())) {
    ++stats_.frames_out;
    return true;
  }
  ++stats_.dropped_out;
  return false;
}

}  // namespace a2ui_transport_ws

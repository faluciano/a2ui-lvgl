// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// One renderer's conversation with one agent over a message transport that
// may drop and come back (a WebSocket). Platform-neutral: it only needs a
// function that sends a text frame, so the same code runs on the device
// (ws_transport.h) and in the host end-to-end test.
//
// Frames (docs/transport.md has the details and examples):
//
//   agent -> renderer   One A2UI v1.0 payload per frame: a message, an array
//                       of messages, or {"messages": [...]}.
//   renderer -> agent   {"message": <renderer-to-agent message>,
//                        "metadata": {...}}. The first frame of every
//                       connection has no "message"; its metadata carries
//                       a2uiRendererCapabilities (scoped to the connection)
//                       and the data model of the previous connection's
//                       surfaces. While a surface has sendDataModel, every
//                       frame carries a2uiRendererDataModel.
//
// Each connection is a new session. When one starts, the surfaces of the
// previous one are deleted, because the agent will create them again and a
// v1.0 renderer must reject a createSurface for an id that still exists.
//
// Not thread-safe: call everything on the task that owns the processor.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "a2ui/error.h"
#include "a2ui/json.h"
#include "a2ui/message_processor.h"

namespace a2ui_transport_ws {

/** Sends one text frame to the agent. Returns false if it was not sent. */
using SendText = std::function<bool(const std::string& frame)>;

struct AgentSessionStats {
  /** Connections started (connected() calls). */
  uint32_t sessions = 0;
  /** Frames from the agent, and how many of them the processor rejected. */
  uint32_t frames_in = 0;
  uint32_t frames_rejected = 0;
  /** Frames sent, including the session start frame. */
  uint32_t frames_out = 0;
  /** Messages for the agent that were dropped: no connection, or the send failed. */
  uint32_t dropped_out = 0;
};

class AgentSession {
 public:
  /**
   * Becomes the processor's outbound listener. Create the processor with
   * MessageProcessorOptions::forward_actions = true so that user actions
   * reach the agent. Destroy the session before the processor.
   */
  AgentSession(a2ui::MessageProcessor& processor, SendText send);
  /** Detaches from the processor (agent calls then fail locally). */
  ~AgentSession();
  AgentSession(const AgentSession&) = delete;
  AgentSession& operator=(const AgentSession&) = delete;

  /**
   * A connection opened. Deletes the previous session's surfaces, then sends
   * the session start frame (capabilities and the previous data model).
   */
  void connected();
  /**
   * The connection closed. Pending agent calls fail with CANCELLED, since
   * their responses can no longer arrive. Surfaces stay on screen until the
   * next connected(). Messages for the agent are dropped until then.
   */
  void disconnected();
  /**
   * A text frame from the agent. Rejected payloads are reported to the agent
   * as `error` messages by the processor and returned here.
   */
  a2ui::Status receive(std::string_view frame);

  bool is_connected() const { return connected_; }
  const AgentSessionStats& stats() const { return stats_; }

  /** The session start frame connected() sends (exposed for tests and logs). */
  a2ui::Json start_frame(const a2ui::Json& previous_data_model) const;

 private:
  void send_message(const a2ui::Json& message);
  bool send_frame(const a2ui::Json& frame);

  a2ui::MessageProcessor& processor_;
  SendText send_;
  bool connected_ = false;
  AgentSessionStats stats_;
};

}  // namespace a2ui_transport_ws

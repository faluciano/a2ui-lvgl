// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// A2UI over a WebSocket on ESP-IDF, with esp_websocket_client.
//
// The client runs its own task. Everything it receives (connected,
// disconnected, complete text frames) goes through a FreeRTOS queue, and
// poll() hands it to the AgentSession on the task that calls it. Call
// poll() from the task that owns the MessageProcessor, the LVGL task: an
// lv_timer is the simplest way. Messages for the agent are sent from that
// task too, straight from the processor's outbound listener.
//
//   a2ui_transport_ws::WsTransportConfig config;
//   config.uri = "ws://192.168.1.2:8765";
//   auto* transport = new a2ui_transport_ws::WsTransport(processor, config);
//   transport->start();
//   lv_timer_create([](lv_timer_t* t) {
//     static_cast<a2ui_transport_ws::WsTransport*>(lv_timer_get_user_data(t))->poll();
//   }, 20, transport);
//
// The client reconnects by itself after an error or a close. Each new
// connection is a new session (see agent_session.h).

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "a2ui/message_processor.h"
#include "a2ui_transport_ws/agent_session.h"
#include "esp_err.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace a2ui_transport_ws {

struct WsTransportConfig {
  /** ws://host:port/path or wss://... Copied by the constructor. */
  std::string uri;
  /** Delay before connecting again after an error or a close. */
  int reconnect_ms = 5000;
  /** Abort a connect, read or write that takes longer than this. */
  int network_timeout_ms = 10000;
  /**
   * A text message from the agent larger than this is dropped, and the
   * connection is restarted so the agent sends its surfaces again.
   */
  size_t max_message_size = 64 * 1024;
  /**
   * Events waiting for poll(). When it is full, the client task waits up to
   * 1 s for room; after that the message is dropped and the connection is
   * restarted, as above.
   */
  size_t queue_length = 16;
  /** How long sending one message to the agent may block the calling task. */
  uint32_t send_timeout_ms = 1000;
  /** The client task. Its stack holds no A2UI processing. */
  int task_stack = 6144;
  int task_priority = 5;
  /**
   * Extra HTTP headers for the handshake, each ending in "\r\n", e.g.
   * "Authorization: Bearer ...\r\n". Copied.
   */
  std::string headers;
  /** PEM certificate of the server (or its CA) for wss://. Not copied. */
  const char* cert_pem = nullptr;
  /** Or esp_crt_bundle_attach, to check wss:// servers against ESP-IDF's CA bundle. */
  esp_err_t (*crt_bundle_attach)(void* conf) = nullptr;
};

class WsTransport {
 public:
  WsTransport(a2ui::MessageProcessor& processor, WsTransportConfig config);
  /** Stops the client. Call from the task that calls poll(), before the processor goes. */
  ~WsTransport();
  WsTransport(const WsTransport&) = delete;
  WsTransport& operator=(const WsTransport&) = delete;

  /** Starts the client task, which connects and keeps reconnecting. */
  esp_err_t start();

  /**
   * Hands up to `max_events` queued events to the session. Call regularly
   * from the task that owns the processor. Returns how many it handled.
   */
  size_t poll(size_t max_events = 8);

  AgentSession& session() { return session_; }
  const AgentSession& session() const { return session_; }
  /** As seen by the session (lags the socket until poll() runs). */
  bool connected() const { return session_.is_connected(); }
  /** Messages from the agent dropped because they were too large or the queue was full. */
  uint32_t dropped_in() const { return dropped_in_.load(); }

 private:
  enum class Kind : uint8_t { Connected, Disconnected, Text };
  struct Event {
    Kind kind;
    std::string* text;  // Kind::Text only; owned by the receiver.
  };

  static void on_event(void* arg, esp_event_base_t base, int32_t id, void* data);
  void handle_event(int32_t id, const esp_websocket_event_data_t* data);
  void handle_data(const esp_websocket_event_data_t* data);
  void post(Kind kind, std::string* text);
  bool send(const std::string& frame);
  void drain();

  WsTransportConfig config_;
  AgentSession session_;
  esp_websocket_client_handle_t client_ = nullptr;
  QueueHandle_t queue_ = nullptr;

  // Client task only.
  bool socket_open_ = false;
  bool assembling_ = false;
  bool oversized_ = false;
  std::string partial_;

  std::atomic<bool> restart_{false};
  std::atomic<uint32_t> dropped_in_{0};
};

}  // namespace a2ui_transport_ws

// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0

#include "a2ui_transport_ws/ws_transport.h"

#include <utility>

#include "esp_log.h"

namespace a2ui_transport_ws {

namespace {

constexpr const char* kTag = "a2ui_ws";
constexpr uint8_t kOpContinuation = 0x0;
constexpr uint8_t kOpText = 0x1;
constexpr uint8_t kOpBinary = 0x2;
// How long the client task waits for room in the queue before it gives up.
constexpr TickType_t kQueueWait = pdMS_TO_TICKS(1000);

}  // namespace

WsTransport::WsTransport(a2ui::MessageProcessor& processor, WsTransportConfig config)
    : config_(std::move(config)),
      session_(processor, [this](const std::string& frame) { return send(frame); }) {
  if (config_.queue_length == 0) config_.queue_length = 1;
  queue_ = xQueueCreate(config_.queue_length, sizeof(Event));
}

WsTransport::~WsTransport() {
  // Stops the client task first, so nothing is posted while the queue drains.
  if (client_ != nullptr) esp_websocket_client_destroy(client_);
  client_ = nullptr;
  drain();
  if (queue_ != nullptr) vQueueDelete(queue_);
}

esp_err_t WsTransport::start() {
  if (queue_ == nullptr) return ESP_ERR_NO_MEM;
  if (client_ != nullptr) return ESP_ERR_INVALID_STATE;

  esp_websocket_client_config_t cfg = {};
  cfg.uri = config_.uri.c_str();
  cfg.reconnect_timeout_ms = config_.reconnect_ms;
  cfg.network_timeout_ms = config_.network_timeout_ms;
  // Reconnect after the agent closes the connection cleanly too, not only after errors.
  cfg.enable_close_reconnect = true;
  cfg.task_stack = config_.task_stack;
  cfg.task_prio = config_.task_priority;
  if (!config_.headers.empty()) cfg.headers = config_.headers.c_str();
  cfg.cert_pem = config_.cert_pem;
  cfg.crt_bundle_attach = config_.crt_bundle_attach;

  client_ = esp_websocket_client_init(&cfg);
  if (client_ == nullptr) return ESP_FAIL;
  esp_err_t err = esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, &WsTransport::on_event, this);
  if (err == ESP_OK) err = esp_websocket_client_start(client_);
  if (err != ESP_OK) {
    esp_websocket_client_destroy(client_);
    client_ = nullptr;
  }
  return err;
}

size_t WsTransport::poll(size_t max_events) {
  if (queue_ == nullptr) return 0;

  if (restart_.exchange(false)) {
    // A message from the agent was lost, so this session's state cannot be
    // trusted. Drop what is queued and start a new session: the agent sends
    // its surfaces again.
    ESP_LOGW(kTag, "a message from the agent was dropped; reconnecting");
    drain();
    session_.disconnected();
    if (client_ != nullptr) esp_websocket_client_close(client_, pdMS_TO_TICKS(config_.send_timeout_ms));
  }

  size_t handled = 0;
  Event event;
  while (handled < max_events && xQueueReceive(queue_, &event, 0) == pdTRUE) {
    ++handled;
    switch (event.kind) {
      case Kind::Connected:
        ESP_LOGI(kTag, "connected to %s", config_.uri.c_str());
        session_.connected();
        break;
      case Kind::Disconnected:
        ESP_LOGI(kTag, "disconnected");
        session_.disconnected();
        break;
      case Kind::Text: {
        a2ui::Status status = session_.receive(*event.text);
        if (!status.ok()) ESP_LOGW(kTag, "agent message rejected: %s", status.error().message.c_str());
        delete event.text;
        break;
      }
    }
  }
  return handled;
}

void WsTransport::drain() {
  if (queue_ == nullptr) return;
  Event event;
  while (xQueueReceive(queue_, &event, 0) == pdTRUE) delete event.text;
}

bool WsTransport::send(const std::string& frame) {
  if (client_ == nullptr || !esp_websocket_client_is_connected(client_)) return false;
  const int sent = esp_websocket_client_send_text(client_, frame.data(), static_cast<int>(frame.size()),
                                                  pdMS_TO_TICKS(config_.send_timeout_ms));
  if (sent != static_cast<int>(frame.size())) {
    ESP_LOGW(kTag, "send failed (%d of %u bytes)", sent, static_cast<unsigned>(frame.size()));
    return false;
  }
  return true;
}

// --- Client task ------------------------------------------------------------

void WsTransport::on_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
  (void)base;
  static_cast<WsTransport*>(arg)->handle_event(id, static_cast<const esp_websocket_event_data_t*>(data));
}

void WsTransport::handle_event(int32_t id, const esp_websocket_event_data_t* data) {
  switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
      socket_open_ = true;
      assembling_ = false;
      partial_.clear();
      post(Kind::Connected, nullptr);
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
      if (socket_open_) {
        socket_open_ = false;
        post(Kind::Disconnected, nullptr);
      }
      break;
    case WEBSOCKET_EVENT_DATA:
      if (data != nullptr) handle_data(data);
      break;
    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGW(kTag, "connection error");
      break;
    default:
      break;
  }
}

void WsTransport::handle_data(const esp_websocket_event_data_t* data) {
  // A message can arrive in several events: a frame larger than the client's
  // buffer comes in pieces (payload_offset), and a fragmented message in
  // several frames (continuation opcode, fin on the last).
  const uint8_t op = data->op_code;
  if (op == kOpBinary) {
    if (data->payload_offset == 0) ESP_LOGW(kTag, "ignoring a binary frame");
    return;
  }
  if (op != kOpText && !(op == kOpContinuation && assembling_)) return;  // Ping, pong, close.

  if (op == kOpText && data->payload_offset == 0) {
    partial_.clear();
    oversized_ = false;
    assembling_ = true;
  }
  const size_t len = data->data_len > 0 ? static_cast<size_t>(data->data_len) : 0;
  if (!oversized_) {
    if (partial_.size() + len > config_.max_message_size) {
      oversized_ = true;
      partial_ = std::string();
    } else {
      partial_.append(data->data_ptr, len);
    }
  }

  const bool frame_done = data->payload_offset + data->data_len >= data->payload_len;
  if (!frame_done || !data->fin) return;
  assembling_ = false;
  if (oversized_) {
    ESP_LOGE(kTag, "message from the agent is larger than %u bytes", static_cast<unsigned>(config_.max_message_size));
    oversized_ = false;
    dropped_in_.fetch_add(1);
    restart_.store(true);
    return;
  }
  post(Kind::Text, new std::string(std::move(partial_)));
  partial_ = std::string();
}

void WsTransport::post(Kind kind, std::string* text) {
  Event event{kind, text};
  if (xQueueSend(queue_, &event, kQueueWait) == pdTRUE) return;
  // The UI task is not keeping up. Losing a message breaks the session, so
  // ask poll() for a new one.
  ESP_LOGE(kTag, "event queue full; dropping a message from the agent");
  delete text;
  dropped_in_.fetch_add(1);
  restart_.store(true);
}

}  // namespace a2ui_transport_ws

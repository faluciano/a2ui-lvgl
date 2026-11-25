// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// A small blocking WebSocket client (RFC 6455) over POSIX sockets, for the
// host end-to-end test only. Text frames, fragmentation, ping and close; no
// TLS, no extensions, and the server's Sec-WebSocket-Accept is not checked.

#pragma once

#include <string>
#include <vector>

namespace e2e {

class WsClient {
 public:
  WsClient() = default;
  ~WsClient();
  WsClient(const WsClient&) = delete;
  WsClient& operator=(const WsClient&) = delete;

  /** Connects to ws://host:port/path and completes the handshake. */
  bool connect(const std::string& url, std::string* error);
  bool is_open() const { return fd_ >= 0; }
  bool send_text(const std::string& text);
  /**
   * Waits up to `timeout_ms` for data and appends every complete text
   * message to `out`. Returns false once the connection is closed.
   */
  bool poll(int timeout_ms, std::vector<std::string>* out);
  /** Sends a close frame and closes the socket. */
  void close();

 private:
  bool send_frame(unsigned char opcode, const std::string& payload);
  bool read_handshake(std::string* error);
  void parse(std::vector<std::string>* out);
  void drop();

  int fd_ = -1;
  std::string in_;       // Bytes received and not parsed yet.
  std::string message_;  // A fragmented text message being assembled.
  bool in_message_ = false;
};

}  // namespace e2e

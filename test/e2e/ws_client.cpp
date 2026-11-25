// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0

#include "ws_client.h"

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <random>

namespace e2e {

namespace {

constexpr unsigned char kOpContinuation = 0x0;
constexpr unsigned char kOpText = 0x1;
constexpr unsigned char kOpClose = 0x8;
constexpr unsigned char kOpPing = 0x9;

std::string base64(const unsigned char* data, size_t len) {
  static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < len; i += 3) {
    uint32_t n = static_cast<uint32_t>(data[i]) << 16;
    if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
    if (i + 2 < len) n |= data[i + 2];
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += i + 1 < len ? kAlphabet[(n >> 6) & 63] : '=';
    out += i + 2 < len ? kAlphabet[n & 63] : '=';
  }
  return out;
}

bool write_all(int fd, const std::string& bytes) {
  size_t done = 0;
  while (done < bytes.size()) {
    const ssize_t n = ::send(fd, bytes.data() + done, bytes.size() - done, MSG_NOSIGNAL);
    if (n <= 0) return false;
    done += static_cast<size_t>(n);
  }
  return true;
}

}  // namespace

WsClient::~WsClient() { drop(); }

bool WsClient::connect(const std::string& url, std::string* error) {
  const std::string scheme = "ws://";
  if (url.compare(0, scheme.size(), scheme) != 0) {
    *error = "only ws:// URLs are supported";
    return false;
  }
  std::string rest = url.substr(scheme.size());
  const size_t slash = rest.find('/');
  const std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
  const std::string authority = rest.substr(0, slash);
  const size_t colon = authority.rfind(':');
  const std::string host = colon == std::string::npos ? authority : authority.substr(0, colon);
  const std::string port = colon == std::string::npos ? "80" : authority.substr(colon + 1);

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* found = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0 || found == nullptr) {
    *error = "cannot resolve " + host;
    return false;
  }
  for (addrinfo* a = found; a != nullptr && fd_ < 0; a = a->ai_next) {
    fd_ = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (fd_ >= 0 && ::connect(fd_, a->ai_addr, a->ai_addrlen) != 0) drop();
  }
  freeaddrinfo(found);
  if (fd_ < 0) {
    *error = "cannot connect to " + authority;
    return false;
  }

  std::random_device rd;
  unsigned char nonce[16];
  for (unsigned char& b : nonce) b = static_cast<unsigned char>(rd());
  const std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + authority +
                              "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " +
                              base64(nonce, sizeof nonce) + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
  if (!write_all(fd_, request)) {
    *error = "cannot send the handshake";
    drop();
    return false;
  }
  return read_handshake(error);
}

bool WsClient::read_handshake(std::string* error) {
  std::string response;
  char buf[1024];
  for (;;) {
    const size_t end = response.find("\r\n\r\n");
    if (end != std::string::npos) {
      in_ = response.substr(end + 4);  // A frame may follow the response at once.
      response.resize(end);
      break;
    }
    pollfd p{fd_, POLLIN, 0};
    if (::poll(&p, 1, 5000) <= 0) {
      *error = "no handshake response";
      drop();
      return false;
    }
    const ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
    if (n <= 0) {
      *error = "connection closed during the handshake";
      drop();
      return false;
    }
    response.append(buf, static_cast<size_t>(n));
  }
  if (response.compare(0, 12, "HTTP/1.1 101") != 0) {
    *error = "handshake refused: " + response.substr(0, response.find("\r\n"));
    drop();
    return false;
  }
  return true;
}

bool WsClient::send_text(const std::string& text) { return send_frame(kOpText, text); }

bool WsClient::send_frame(unsigned char opcode, const std::string& payload) {
  if (fd_ < 0) return false;
  std::string frame;
  frame += static_cast<char>(0x80 | opcode);  // FIN
  const uint64_t len = payload.size();
  if (len < 126) {
    frame += static_cast<char>(0x80 | len);  // Client frames are masked.
  } else if (len <= 0xFFFF) {
    frame += static_cast<char>(0x80 | 126);
    frame += static_cast<char>((len >> 8) & 0xFF);
    frame += static_cast<char>(len & 0xFF);
  } else {
    frame += static_cast<char>(0x80 | 127);
    for (int shift = 56; shift >= 0; shift -= 8) frame += static_cast<char>((len >> shift) & 0xFF);
  }
  static std::mt19937 rng{std::random_device{}()};
  unsigned char mask[4];
  for (unsigned char& b : mask) b = static_cast<unsigned char>(rng());
  frame.append(reinterpret_cast<const char*>(mask), 4);
  for (size_t i = 0; i < payload.size(); ++i) frame += static_cast<char>(payload[i] ^ mask[i % 4]);
  if (!write_all(fd_, frame)) {
    drop();
    return false;
  }
  return true;
}

bool WsClient::poll(int timeout_ms, std::vector<std::string>* out) {
  parse(out);
  if (fd_ < 0) return false;
  pollfd p{fd_, POLLIN, 0};
  if (::poll(&p, 1, timeout_ms) > 0) {
    char buf[8192];
    const ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
    if (n <= 0) {
      drop();
      return false;
    }
    in_.append(buf, static_cast<size_t>(n));
    parse(out);
  }
  return fd_ >= 0;
}

void WsClient::parse(std::vector<std::string>* out) {
  for (;;) {
    if (in_.size() < 2) return;
    const auto* b = reinterpret_cast<const unsigned char*>(in_.data());
    const bool fin = (b[0] & 0x80) != 0;
    const unsigned char opcode = b[0] & 0x0F;
    const bool masked = (b[1] & 0x80) != 0;
    uint64_t len = b[1] & 0x7F;
    size_t pos = 2;
    if (len == 126) {
      if (in_.size() < 4) return;
      len = (static_cast<uint64_t>(b[2]) << 8) | b[3];
      pos = 4;
    } else if (len == 127) {
      if (in_.size() < 10) return;
      len = 0;
      for (int i = 0; i < 8; ++i) len = (len << 8) | b[2 + i];
      pos = 10;
    }
    const size_t mask_at = pos;
    if (masked) pos += 4;
    if (in_.size() < pos + len) return;
    std::string payload = in_.substr(pos, static_cast<size_t>(len));
    if (masked) {
      for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ in_[mask_at + i % 4]);
    }
    in_.erase(0, pos + static_cast<size_t>(len));

    if (opcode == kOpText || (opcode == kOpContinuation && in_message_)) {
      if (opcode == kOpText) message_.clear();
      message_ += payload;
      in_message_ = !fin;
      if (fin) out->push_back(std::move(message_));
    } else if (opcode == kOpPing) {
      send_frame(0xA, payload);
    } else if (opcode == kOpClose) {
      send_frame(kOpClose, payload.substr(0, 2));
      drop();
      return;
    }
  }
}

void WsClient::close() {
  if (fd_ < 0) return;
  send_frame(kOpClose, std::string("\x03\xE8", 2));  // 1000, normal closure.
  drop();
}

void WsClient::drop() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  in_.clear();
  message_.clear();
  in_message_ = false;
}

}  // namespace e2e

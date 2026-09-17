#pragma once

// A minimal blocking RESP2 client for tests. Replies are rendered as strings so
// assertions read like a redis-cli session:
//
//   "+OK"   "-STALE ..."   ":42"   "$payload"   "nil"   "[:1, $emails, ...]"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "common/fd.h"

namespace baton {

class RespClient {
 public:
  explicit RespClient(uint16_t port) {
    fd_.reset(::socket(AF_INET, SOCK_STREAM, 0));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd_.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
      throw std::runtime_error("RespClient: connect failed");
    }
    const int on = 1;
    ::setsockopt(fd_.get(), IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
  }

  static std::string encode(const std::vector<std::string>& args) {
    std::string out = "*" + std::to_string(args.size()) + "\r\n";
    for (const std::string& arg : args) {
      out += "$" + std::to_string(arg.size()) + "\r\n" + arg + "\r\n";
    }
    return out;
  }

  void send_raw(std::string_view bytes) {
    while (!bytes.empty()) {
      const ssize_t n = ::send(fd_.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL);
      if (n <= 0) throw std::runtime_error("RespClient: send failed");
      bytes.remove_prefix(static_cast<size_t>(n));
    }
  }

  void send(const std::vector<std::string>& args) { send_raw(encode(args)); }

  // Sends a command and waits for its reply.
  std::string command(const std::vector<std::string>& args, int timeout_ms = 10'000) {
    send(args);
    return read_reply(timeout_ms);
  }

  // The next reply, rendered. "<timeout>" if nothing complete arrives in time,
  // "<closed>" if the server closed the connection.
  std::string read_reply(int timeout_ms = 10'000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
      size_t pos = 0;
      if (const auto rendered = render(pos)) {
        buffer_.erase(0, pos);
        return *rendered;
      }
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - std::chrono::steady_clock::now())
                            .count();
      if (left <= 0) return "<timeout>";
      const int status = fill(static_cast<int>(left));
      if (status == 0) return "<timeout>";
      if (status < 0) return "<closed>";
    }
  }

  // True if no byte at all arrives within `wait_ms`: the server is holding the
  // reply back (or has nothing to say).
  bool stays_silent(int wait_ms) {
    if (!buffer_.empty()) return false;
    return fill(wait_ms) == 0;
  }

  // Reads until the server closes the connection; true if it did so in time.
  bool wait_for_close(int timeout_ms = 10'000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      if (fill(100) < 0) return true;
      buffer_.clear();
    }
    return false;
  }

  void close() { fd_.reset(); }

 private:
  // Waits for data and appends it. 1: got bytes, 0: timeout, -1: closed or error.
  int fill(int timeout_ms) {
    pollfd p{.fd = fd_.get(), .events = POLLIN, .revents = 0};
    if (::poll(&p, 1, timeout_ms) <= 0) return 0;
    char chunk[65536];
    const ssize_t n = ::recv(fd_.get(), chunk, sizeof(chunk), 0);
    if (n <= 0) return -1;
    buffer_.append(chunk, static_cast<size_t>(n));
    return 1;
  }

  // Renders the value at buffer_[pos...]; nullopt if it is not complete yet.
  std::optional<std::string> render(size_t& pos) const {
    const size_t line_end = buffer_.find("\r\n", pos);
    if (line_end == std::string::npos) return std::nullopt;
    const char type = buffer_[pos];
    const std::string line = buffer_.substr(pos + 1, line_end - pos - 1);
    pos = line_end + 2;
    switch (type) {
      case '+':
        return "+" + line;
      case '-':
        return "-" + line;
      case ':':
        return ":" + line;
      case '$': {
        const long length = std::stol(line);
        if (length < 0) return std::string("nil");
        if (buffer_.size() < pos + static_cast<size_t>(length) + 2) return std::nullopt;
        std::string value = "$" + buffer_.substr(pos, static_cast<size_t>(length));
        pos += static_cast<size_t>(length) + 2;
        return value;
      }
      case '*': {
        const long count = std::stol(line);
        if (count < 0) return std::string("nil");
        std::string out = "[";
        for (long i = 0; i < count; ++i) {
          const auto element = render(pos);
          if (!element) return std::nullopt;
          if (i > 0) out += ", ";
          out += *element;
        }
        return out + "]";
      }
      default:
        throw std::runtime_error("RespClient: server sent invalid RESP");
    }
  }

  Fd fd_;
  std::string buffer_;
};

}  // namespace baton

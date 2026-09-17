#pragma once

// Thin wrappers over the BSD socket calls baton needs. Everything returned is
// non-blocking and close-on-exec.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "common/fd.h"
#include "common/result.h"

namespace baton {

// Listens on an IPv4 or IPv6 literal address ("127.0.0.1", "0.0.0.0", "::1").
// Host names are not resolved: what baton binds to should never depend on DNS.
// Port 0 picks a free port; see local_port().
Result<Fd> listen_tcp(const std::string& address, uint16_t port, int backlog = 511);

Result<uint16_t> local_port(int fd);

// Accepts one pending connection. nullopt means there is none right now.
// The socket has TCP_NODELAY set: replies are small and latency matters.
Result<std::optional<Fd>> accept_connection(int listen_fd);

// A pair of connected non-blocking descriptors used to wake the event loop from
// another thread or a signal handler: write a byte to `second`, read `first`.
Result<std::pair<Fd, Fd>> make_wakeup_pipe();

enum class IoStatus : uint8_t { kOk, kWouldBlock, kClosed, kError };

struct IoResult {
  IoStatus status = IoStatus::kOk;
  size_t bytes = 0;
  int error = 0;  // errno when status == kError
};

// Reads up to `capacity` bytes. kClosed: the peer shut down cleanly.
IoResult read_some(int fd, char* buffer, size_t capacity);
// Writes as much of `data` as the socket accepts, without raising SIGPIPE.
IoResult write_some(int fd, std::string_view data);

}  // namespace baton

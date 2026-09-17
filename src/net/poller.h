#pragma once

// Readiness notification behind one interface: epoll on Linux, kqueue on macOS.
// Level-triggered on both, which keeps the event loop simple: if a socket still
// has data (or room) next time around, the poller says so again.

#include <memory>
#include <vector>

#include "common/result.h"

namespace baton {

struct PollEvent {
  int fd = -1;
  bool readable = false;
  bool writable = false;
  bool hangup = false;  // error or peer closed: the owner should read (to see EOF) or close
};

class Poller {
 public:
  Poller() = default;
  Poller(const Poller&) = delete;
  Poller& operator=(const Poller&) = delete;
  virtual ~Poller() = default;

  static Result<std::unique_ptr<Poller>> create();

  virtual Status add(int fd, bool want_read, bool want_write) = 0;
  virtual Status modify(int fd, bool want_read, bool want_write) = 0;
  // Must be called before the descriptor is closed.
  virtual void remove(int fd) = 0;

  // Blocks for at most timeout_ms (-1: until something happens) and appends the
  // ready descriptors to `events`. Being interrupted by a signal is not an error.
  virtual Status wait(int timeout_ms, std::vector<PollEvent>& events) = 0;
};

}  // namespace baton

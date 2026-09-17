#pragma once

// Per-client state. Owned and touched only by the event loop thread.

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "common/clock.h"
#include "common/fd.h"
#include "common/types.h"
#include "net/resp.h"
#include "sched/timing_wheel.h"

namespace baton {

struct Connection {
  Connection(uint64_t connection_id, Fd socket, RespLimits limits)
      : id(connection_id), fd(std::move(socket)), parser(limits) {}

  uint64_t id;
  Fd fd;

  // --- input ----------------------------------------------------------------------
  std::string in;        // bytes read from the socket
  size_t in_offset = 0;  // in[in_offset..] has not been parsed yet
  RespParser parser;

  // --- replies waiting for durability ------------------------------------------------
  // `gated` holds reply bytes that may not be sent yet. Each mark says: the bytes
  // up to `end` may go once `lsn` is committed. Marks are in LSN order, and
  // bytes only ever leave from the front, so replies cannot overtake each other.
  struct Mark {
    Lsn lsn = 0;
    size_t end = 0;
  };
  std::string gated;
  std::deque<Mark> marks;

  // --- replies released to the socket ------------------------------------------------
  std::string out;
  size_t out_offset = 0;  // out[out_offset..] still has to be written
  bool want_write = false;
  bool dirty = false;  // has replies to flush at the end of this loop iteration

  // What the poller currently watches for, to skip redundant system calls.
  bool registered_read = false;
  bool registered_write = false;

  // --- session -------------------------------------------------------------------------
  bool authenticated = false;
  bool close_after_flush = false;  // protocol error or QUIT: reply, then close
  bool read_paused = false;        // backpressure: the log is behind
  std::string name;

  // --- a parked RESERVE ----------------------------------------------------------------
  struct Blocked {
    std::vector<std::string> queues;
    DurationMs lease_ms = 0;
    TimerHandle timeout;
  };
  std::optional<Blocked> blocked;

  size_t unsent_bytes() const { return gated.size() + (out.size() - out_offset); }
};

}  // namespace baton

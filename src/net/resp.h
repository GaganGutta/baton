#pragma once

// RESP2: the request parser and the reply writer. Design: docs/design.md 6.3.
//
// The parser is what stands between the network and everything else, so it is
// paranoid by construction:
//   - every length is validated before it is used, and nothing is ever
//     allocated or skipped on the say-so of an unvalidated number;
//   - requests are arrays of bulk strings, full stop (no inline commands, no
//     nested arrays);
//   - it is incremental: fed any split of a byte stream it produces the same
//     requests (the fuzz target checks exactly this).
//
// Normal requests are zero-copy: args are views into the caller's buffer, and an
// incomplete request consumes nothing (the caller retries with more data). An
// argument larger than max_bulk_bytes switches the parser into a streaming
// discard mode: the rest of that request is consumed as it arrives without ever
// being buffered, and it is reported as `oversized` so the caller can answer
// with a clean error and keep the connection.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace baton {

struct RespLimits {
  size_t max_args = 1024;
  // Arguments above this are skipped, not buffered (request.oversized = true).
  size_t max_bulk_bytes = (size_t{1} << 20U) + 1024;
  // Arguments above this are a protocol error.
  uint64_t hard_max_bulk_bytes = uint64_t{512} << 20U;
  // A whole (non-oversized) request above this is a protocol error. Bounds what a
  // connection ever has to buffer: without it, 1,024 arguments just under
  // max_bulk_bytes would add up to a gigabyte.
  size_t max_request_bytes = (size_t{1} << 20U) + (size_t{64} << 10U);
};

struct RespRequest {
  std::vector<std::string_view> args;  // empty when oversized
  bool oversized = false;
};

class RespParser {
 public:
  enum class Status : uint8_t {
    kRequest,   // `request` is complete; `consumed` bytes of the input belong to it
    kNeedMore,  // feed more data; `consumed` bytes (possibly 0) are already dealt with
    kError,     // protocol error described by `error`; the stream is unusable
  };

  struct Outcome {
    Status status = Status::kNeedMore;
    size_t consumed = 0;
  };

  explicit RespParser(RespLimits limits = {}) : limits_(limits) {}

  // Parses from the start of `data`. The caller drops `consumed` bytes from the
  // front of its buffer after every call, whatever the status. On kRequest the
  // views in `request` point into `data` and stay valid until the caller
  // modifies its buffer.
  Outcome parse(std::string_view data, RespRequest& request, std::string& error);

  // True while the rest of an oversized request is being skipped.
  bool discarding() const { return discarding_; }

 private:
  Outcome parse_discarding(std::string_view data, size_t pos, RespRequest& request,
                           std::string& error);

  RespLimits limits_;
  bool discarding_ = false;
  size_t discard_args_left_ = 0;     // arguments still to come after the current one
  uint64_t discard_bytes_left_ = 0;  // of the bulk being skipped, including its CRLF
};

// --- replies ----------------------------------------------------------------------
// Append one RESP2 value to `out`.

void resp_simple(std::string& out, std::string_view text);  // +text
void resp_error(std::string& out, std::string_view code, std::string_view message);
void resp_integer(std::string& out, int64_t value);
void resp_bulk(std::string& out, std::string_view data);
void resp_null_array(std::string& out);
void resp_array_header(std::string& out, size_t count);  // followed by `count` values

}  // namespace baton

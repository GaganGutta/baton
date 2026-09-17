// Fuzz target: the RESP2 request parser, the first thing untrusted bytes meet.
//
// The first input byte chooses a chunk size; the rest is the byte stream.
// Properties:
//   - no crash, over-read or sanitizer report on any input;
//   - the parser never asks the caller to hold more than a bounded buffer: an
//     oversized argument is skipped as it streams in, not accumulated;
//   - splitting the stream differently never changes the result: fed in chunks
//     or all at once, the same requests (or the same error) come out.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/check.h"
#include "net/resp.h"

namespace {

struct Drained {
  std::vector<std::vector<std::string>> requests;
  std::vector<bool> oversized;
  bool failed = false;
  size_t max_buffered = 0;
};

Drained drain(const std::string& stream, size_t chunk, const baton::RespLimits& limits) {
  Drained out;
  baton::RespParser parser(limits);
  std::string buffer;
  std::string error;
  for (size_t offset = 0; offset < stream.size() && !out.failed; offset += chunk) {
    buffer.append(stream, offset, chunk);
    out.max_buffered = std::max(out.max_buffered, buffer.size());
    for (;;) {
      baton::RespRequest request;
      const auto outcome = parser.parse(buffer, request, error);
      BATON_CHECK(outcome.consumed <= buffer.size());
      if (outcome.status == baton::RespParser::Status::kRequest) {
        BATON_CHECK(request.oversized == request.args.empty());
        std::vector<std::string> args;
        for (const std::string_view arg : request.args) {
          // Views must lie inside the buffer that was parsed.
          BATON_CHECK(arg.data() >= buffer.data());
          BATON_CHECK(arg.data() + arg.size() <= buffer.data() + buffer.size());
          args.emplace_back(arg);
        }
        out.requests.push_back(std::move(args));
        out.oversized.push_back(request.oversized);
      }
      if (outcome.status == baton::RespParser::Status::kError) out.failed = true;
      buffer.erase(0, outcome.consumed);
      if (outcome.status != baton::RespParser::Status::kRequest) break;
    }
  }
  return out;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 1) return 0;
  const size_t chunk = 1 + (data[0] % 64);
  const std::string stream(reinterpret_cast<const char*>(data + 1), size - 1);

  baton::RespLimits limits;
  limits.max_args = 16;
  limits.max_bulk_bytes = 64;  // small, so the discard path is exercised constantly
  limits.hard_max_bulk_bytes = 4096;
  limits.max_request_bytes = 512;

  const Drained chunked = drain(stream, chunk, limits);
  const Drained whole = drain(stream, std::max<size_t>(stream.size(), 1), limits);

  BATON_CHECK(chunked.requests == whole.requests, "chunking changed the parsed requests");
  BATON_CHECK(chunked.oversized == whole.oversized);
  BATON_CHECK(chunked.failed == whole.failed, "chunking changed whether the stream is valid");

  // A chunked reader never has to hold more than one maximal normal request,
  // a partial header line and the chunk that completed it.
  const size_t bound = limits.max_request_bytes + 96 + chunk;
  BATON_CHECK(chunked.max_buffered <= bound, "buffered {} bytes, bound {}", chunked.max_buffered,
              bound);
  return 0;
}

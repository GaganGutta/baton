#include <benchmark/benchmark.h>

#include <algorithm>
#include <string>
#include <string_view>

#include "net/resp.h"

namespace {

std::string enqueue_request(size_t payload_bytes) {
  const std::string payload(payload_bytes, 'x');
  return "*5\r\n$7\r\nENQUEUE\r\n$20\r\nemails.transactional\r\n$" +
         std::to_string(payload.size()) + "\r\n" + payload +
         "\r\n$3\r\nKEY\r\n$13\r\norder-1234567\r\n";
}

// What the event loop pays per request before it knows what the request is: a
// pipeline of ENQUEUEs as one read() would deliver it, parsed zero-copy.
void BM_RespParsePipelinedEnqueue(benchmark::State& state) {
  const std::string one = enqueue_request(static_cast<size_t>(state.range(0)));
  std::string pipeline;
  constexpr int kRequests = 64;
  for (int i = 0; i < kRequests; ++i) pipeline += one;

  baton::RespParser parser;
  baton::RespRequest request;
  std::string error;
  for (auto _ : state) {
    std::string_view rest = pipeline;
    int parsed = 0;
    while (!rest.empty()) {
      const auto outcome = parser.parse(rest, request, error);
      if (outcome.status != baton::RespParser::Status::kRequest) break;
      rest.remove_prefix(outcome.consumed);
      ++parsed;
    }
    if (parsed != kRequests) state.SkipWithError("the pipeline did not parse");
    benchmark::DoNotOptimize(request.args.data());
  }
  state.SetItemsProcessed(state.iterations() * kRequests);
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(pipeline.size()));
}
BENCHMARK(BM_RespParsePipelinedEnqueue)->Arg(100)->Arg(1024)->Arg(64 * 1024);

// The same bytes arriving in small pieces: the parser restarts at the beginning
// of the incomplete request each time (it keeps no partial state), so this is
// the price of that simplicity on a slow client.
void BM_RespParseFragmentedEnqueue(benchmark::State& state) {
  const std::string one = enqueue_request(1024);
  const auto fragment = static_cast<size_t>(state.range(0));
  baton::RespParser parser;
  baton::RespRequest request;
  std::string error;
  for (auto _ : state) {
    bool done = false;
    for (size_t have = fragment; !done; have = std::min(have + fragment, one.size())) {
      const auto outcome = parser.parse(std::string_view(one).substr(0, have), request, error);
      done = outcome.status == baton::RespParser::Status::kRequest;
      if (have == one.size() && !done) {
        state.SkipWithError("the request did not parse");
        break;
      }
    }
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RespParseFragmentedEnqueue)->Arg(16)->Arg(256);

}  // namespace

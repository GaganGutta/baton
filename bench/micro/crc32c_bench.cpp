#include <benchmark/benchmark.h>

#include <string>

#include "common/crc32c.h"

namespace {

std::string make_data(size_t size) {
  std::string data(size, '\0');
  for (size_t i = 0; i < size; ++i) data[i] = static_cast<char>((i * 131) ^ (i >> 3U));
  return data;
}

void BM_Crc32cSoftware(benchmark::State& state) {
  const std::string data = make_data(static_cast<size_t>(state.range(0)));
  for (auto _ : state) benchmark::DoNotOptimize(baton::detail::crc32c_software(0, data));
  state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Crc32cSoftware)->Arg(64)->Arg(1024)->Arg(64 * 1024);

void BM_Crc32cHardware(benchmark::State& state) {
  if (!baton::detail::crc32c_hardware_available()) {
    state.SkipWithMessage("no hardware CRC32C on this CPU");
    return;
  }
  const std::string data = make_data(static_cast<size_t>(state.range(0)));
  for (auto _ : state) benchmark::DoNotOptimize(baton::detail::crc32c_hardware(0, data));
  state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Crc32cHardware)->Arg(64)->Arg(1024)->Arg(64 * 1024);

}  // namespace

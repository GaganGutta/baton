#include <benchmark/benchmark.h>

#include <string>

#include "log/format.h"
#include "state/records.h"

namespace {

baton::JobEnqueued sample_enqueue(const std::string& payload) {
  baton::JobEnqueued r;
  r.id = 123'456'789;
  r.queue = "emails.transactional";
  r.payload = payload;
  r.priority = 1;
  r.run_at = baton::WallTime{1'700'000'000'000};
  r.max_attempts = 10;
  r.backoff_base_ms = 1'000;
  r.backoff_cap_ms = 600'000;
  r.idem_key = "order-1234567";
  r.idem_expires_at = baton::WallTime{1'700'086'400'000};
  r.at = baton::WallTime{1'700'000'000'000};
  return r;
}

// What ENQUEUE costs on the way into the log: payload encoding, framing, CRC32C.
void BM_EncodeEnqueueIntoLogRecord(benchmark::State& state) {
  const std::string payload(static_cast<size_t>(state.range(0)), 'x');
  const baton::Record record = sample_enqueue(payload);
  std::string scratch;
  std::string batch;
  for (auto _ : state) {
    scratch.clear();
    batch.clear();
    baton::encode_record(record, scratch);
    baton::append_record(batch, 42, static_cast<uint8_t>(baton::type_of(record)), scratch);
    benchmark::DoNotOptimize(batch.data());
  }
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(batch.size()));
}
BENCHMARK(BM_EncodeEnqueueIntoLogRecord)->Arg(100)->Arg(1024)->Arg(64 * 1024);

// What recovery costs per record: framing check, CRC32C, payload decoding.
void BM_DecodeEnqueueFromLogRecord(benchmark::State& state) {
  const std::string payload(static_cast<size_t>(state.range(0)), 'x');
  std::string scratch;
  baton::encode_record(sample_enqueue(payload), scratch);
  std::string framed;
  baton::append_record(framed, 42, 1, scratch);
  for (auto _ : state) {
    const baton::RecordParseResult parsed = baton::parse_record(framed);
    const auto record = baton::decode_record(parsed.record.type, parsed.record.payload);
    benchmark::DoNotOptimize(record.ok());
  }
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(framed.size()));
}
BENCHMARK(BM_DecodeEnqueueFromLogRecord)->Arg(100)->Arg(1024)->Arg(64 * 1024);

}  // namespace

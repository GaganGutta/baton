// Writes seed inputs for the fuzz targets, so they start from valid structures
// instead of having to discover the formats byte by byte.
//
//   fuzz_make_seeds <corpus-root>   creates <corpus-root>/<target>/seed-*

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "common/clock.h"
#include "log/format.h"
#include "state/engine.h"
#include "state/state.h"
#include "testing/memory_record_sink.h"

namespace {

// A short but varied life of a server: every record type that handlers produce.
void run_traffic(baton::Engine& engine, baton::FakeClock& clock) {
  using baton::EnqueueRequest;
  const std::vector<std::string_view> queues = {"emails", "reports"};
  (void)engine.enqueue(EnqueueRequest{.queue = "emails", .payload = "a", .idem_key = "k1"});
  (void)engine.enqueue(EnqueueRequest{.queue = "emails", .payload = "b", .priority = 5});
  (void)engine.enqueue(EnqueueRequest{.queue = "reports", .payload = "c", .delay_ms = 5'000});
  (void)engine.enqueue(EnqueueRequest{.queue = "reports", .payload = "d", .max_attempts = 1});
  for (int round = 0; round < 3; ++round) {
    auto lease = engine.reserve(queues, 1'000);
    if (!lease.ok() || !lease->has_value()) break;
    const baton::Reservation r = **lease;
    if (round == 0) (void)engine.ack(r.id, r.token);
    if (round == 1) (void)engine.heartbeat(r.id, r.token, 2'000);
    if (round == 2) (void)engine.fail({.id = r.id, .token = r.token, .error = "boom"});
  }
  (void)engine.cancel(3);
  clock.advance(10'000);  // expires the lease that was only heartbeated
  engine.tick();
}

void write_seed(const std::filesystem::path& dir, const std::string& name,
                const std::string& bytes) {
  std::filesystem::create_directories(dir);
  std::ofstream out(dir / name, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string segment_with_records(int count) {
  std::string data;
  baton::append_segment_header(data, 1);
  for (int i = 1; i <= count; ++i) {
    baton::append_record(data, static_cast<baton::Lsn>(i), static_cast<uint8_t>(i),
                         "payload-" + std::to_string(i));
  }
  return data;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: fuzz_make_seeds <corpus-root>\n";
    return 2;
  }
  const std::filesystem::path root = argv[1];

  const std::string three = segment_with_records(3);
  write_seed(root / "fuzz_log_segment", "seed-empty-segment", segment_with_records(0));
  write_seed(root / "fuzz_log_segment", "seed-three-records", three);
  write_seed(root / "fuzz_log_segment", "seed-torn-tail", three.substr(0, three.size() - 5));
  write_seed(root / "fuzz_log_segment", "seed-torn-header", three.substr(0, 20));

  // Damage programs: [records per segment, record count, ops...]
  write_seed(root / "fuzz_log_damage", "seed-pristine", std::string("\x03\x0A", 2));
  write_seed(root / "fuzz_log_damage", "seed-flip-mid",
             std::string("\x03\x0A\x00\x01\x00\x30\x02", 7));
  write_seed(root / "fuzz_log_damage", "seed-truncate-last",
             std::string("\x03\x0A\x01\x03\x00\x40", 6));
  write_seed(root / "fuzz_log_damage", "seed-delete-middle", std::string("\x02\x0A\x03\x02", 4));
  write_seed(root / "fuzz_log_damage", "seed-append-garbage",
             std::string("\x04\x0C\x04\x02\x20\x07", 6));

  // State machine targets: records and a serialized state from real traffic.
  baton::FakeClock clock;
  baton::MemoryRecordSink sink;
  baton::State state;
  baton::Engine engine(state, sink, clock, baton::EngineOptions{}, /*rng_seed=*/7);
  run_traffic(engine, clock);

  std::string sequence;
  for (size_t i = 0; i < sink.entries().size(); ++i) {
    const auto& entry = sink.entries()[i];
    std::string one(1, static_cast<char>(entry.type));
    write_seed(root / "fuzz_record_decode", "seed-record-" + std::to_string(i),
               one + entry.payload);
    sequence += one;
    sequence.push_back(static_cast<char>(entry.payload.size() & 0xFFU));
    sequence.push_back(static_cast<char>((entry.payload.size() >> 8U) & 0xFFU));
    sequence += entry.payload;
  }
  write_seed(root / "fuzz_state_apply", "seed-traffic", sequence);

  std::string serialized;
  state.serialize(serialized);
  write_seed(root / "fuzz_state_deserialize", "seed-traffic", serialized);
  write_seed(root / "fuzz_state_deserialize", "seed-empty", [] {
    std::string empty;
    baton::State().serialize(empty);
    return empty;
  }());
  return 0;
}

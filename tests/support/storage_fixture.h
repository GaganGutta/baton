#pragma once

// Helpers for snapshot and recovery tests: realistic traffic through the Engine,
// and a durable log in SimFs built from the records that traffic produced (no
// log thread involved, so every file-system operation in a test is the
// snapshot code's own).

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "common/clock.h"
#include "log/format.h"
#include "state/engine.h"
#include "state/image.h"
#include "state/state.h"
#include "support/engine_fixture.h"
#include "testing/memory_record_sink.h"
#include "testing/sim_fs.h"

namespace baton {

inline constexpr const char* kStorageDir = "data";

// Jobs in every state, several queues, keys, errors, binary and empty payloads.
inline void run_mixed_traffic(Engine& engine, FakeClock& clock, int rounds) {
  const std::vector<std::string_view> queues = {"a", "b"};
  for (int i = 0; i < rounds; ++i) {
    const std::string key = "key-" + std::to_string(i);
    (void)engine.enqueue({.queue = i % 2 == 0 ? "a" : "b",
                          .payload = std::string(static_cast<size_t>(i % 50), 'p'),
                          .priority = i % 3,
                          .delay_ms = i % 5 == 0 ? 60'000 : 0,
                          .idem_key = i % 4 == 0 ? std::string_view(key) : std::string_view()});
    if (i % 3 == 0) {
      auto lease = engine.reserve(queues, 30'000);
      if (lease.ok() && lease->has_value()) {
        const Reservation r = **lease;
        if (i % 9 == 0) (void)engine.ack(r.id, r.token);
        if (i % 9 == 3) (void)engine.fail({.id = r.id, .token = r.token, .error = "boom"});
        if (i % 9 == 6) {
          (void)engine.fail({.id = r.id, .token = r.token, .error = "dead", .no_retry = true});
        }
      }
    }
    if (i % 17 == 0) (void)engine.cancel(static_cast<JobId>(1 + (i / 2)));
    clock.advance(7);
    engine.tick();
  }
}

// Writes entries [first, last) of `sink` as durable log segments holding at most
// `per_segment` records each. LSNs are the entries' own (index + 1).
inline void write_log(SimFs& fs, const MemoryRecordSink& sink, size_t first, size_t last,
                      size_t per_segment) {
  std::string segment;
  Lsn segment_first = 0;
  const auto flush = [&] {
    if (segment_first == 0) return;
    fs.write_file(join_path(kStorageDir, segment_file_name(segment_first)), segment);
    segment.clear();
    segment_first = 0;
  };
  for (size_t i = first; i < last; ++i) {
    const auto& entry = sink.entries()[i];
    if (segment_first == 0) {
      segment_first = entry.lsn;
      append_segment_header(segment, segment_first);
    }
    append_record(segment, entry.lsn, static_cast<uint8_t>(entry.type), entry.payload);
    if ((i - first + 1) % per_segment == 0) flush();
  }
  flush();
}

// The durable state after the first `count` records, as an image.
inline StateImage image_after(const MemoryRecordSink& sink, size_t count) {
  State prefix;
  prefix.begin_replay();
  EXPECT_TRUE(sink.replay_into(prefix, 0, count).ok());
  return prefix.capture_image();
}

}  // namespace baton

// storage_bench: what snapshots cost and what they buy (docs/design.md 8.6).
//
//   storage_bench --dir <directory on a real disk> [--jobs 10000,100000] [--payload 256]
//
// Snapshot cost, for N live jobs (5% leased, 10% delayed, 8 queues):
//   pause   State::capture_image() - the only time the event loop stands still
//   write   write_snapshot(): serialize, write, fsync, rename, fsync the directory
//   load    load_snapshot() and rebuilding the derived state
//
// Recovery, for a log in which each of N jobs was enqueued, leased and
// acknowledged (3N records), recover_state() from the log alone and from a
// snapshot taken at the end of the same log. Two histories:
//   retained  all N finished jobs are still within their retention period, so
//             the snapshot is as large as it can get relative to the log
//   churned   the N jobs were spread over ten retention periods, so only the
//             last tenth is still in memory - the normal case for a server
//             that has been up for a while
//
// The pause is sampled 9 times (median and worst are reported); every other
// number is the median of 5 runs. The files were just written, so reads come
// from the page cache: these are CPU costs, not cold-disk costs.
//
// Prints Markdown tables; docs/benchmarks.md quotes them verbatim. Run it
// through bench/run_storage_bench.sh, which also records the environment.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/check.h"
#include "common/clock.h"
#include "common/logging.h"
#include "common/posix_fs.h"
#include "log/log_writer.h"
#include "log/recovery.h"
#include "snapshot/recovery.h"
#include "snapshot/snapshot.h"
#include "state/engine.h"
#include "state/state.h"

namespace baton {
namespace {

constexpr int kPauseSamples = 9;
constexpr int kSamples = 5;  // everything else: the median of this many runs
constexpr size_t kQueues = 8;

class Stopwatch {
 public:
  double ms() const {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_)
        .count();
  }

 private:
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

// `run` returns what it built, and the clock is read before that is destroyed:
// tearing down a million-job state is not part of loading one.
template <typename F>
double median_ms(F&& run) {
  std::vector<double> samples;
  for (int i = 0; i < kSamples; ++i) {
    const Stopwatch watch;
    [[maybe_unused]] const auto built = run();
    samples.push_back(watch.ms());
  }
  std::ranges::sort(samples);
  return samples[samples.size() / 2];
}

// For building a state without a log.
class CountingSink final : public RecordSink {
 public:
  Lsn append(RecordType /*type*/, std::string_view /*payload*/) override { return ++lsn_; }

 private:
  Lsn lsn_ = 0;
};

// The engine's records, written by the real log writer the way the server does.
class WriterSink final : public RecordSink {
 public:
  explicit WriterSink(LogWriter& writer) : writer_(writer) {}

  Lsn append(RecordType type, std::string_view payload) override {
    const Lsn lsn = writer_.append(static_cast<uint8_t>(type), payload);
    if (++unflushed_ >= 512) {
      unflushed_ = 0;
      writer_.flush();
      while (writer_.backlog_bytes() > (uint64_t{32} << 20U)) std::this_thread::yield();
    }
    return lsn;
  }

 private:
  LogWriter& writer_;
  size_t unflushed_ = 0;
};

std::vector<std::string> queue_names() {
  std::vector<std::string> names;
  for (size_t i = 0; i < kQueues; ++i) names.push_back(std::format("queue-{}", i));
  return names;
}

std::string fresh_dir(const std::string& root, std::string_view name) {
  const std::filesystem::path path = std::filesystem::path(root) / name;
  std::filesystem::remove_all(path);
  std::filesystem::create_directories(path);
  return path.string();
}

// --- snapshot cost ------------------------------------------------------------------------

void measure_snapshot_cost(PosixFs& fs, const std::string& root, size_t jobs, size_t payload) {
  FakeClock clock;
  CountingSink sink;
  State state;
  Engine engine(state, sink, clock, EngineOptions{}, /*rng_seed=*/1);

  const std::vector<std::string> names = queue_names();
  const std::string body(payload, 'x');
  for (size_t i = 0; i < jobs; ++i) {
    const auto id = engine.enqueue({.queue = names[i % kQueues],
                                    .payload = body,
                                    .priority = static_cast<int32_t>(i % 3),
                                    .delay_ms = i % 10 == 0 ? 3'600'000 : 0});
    BATON_CHECK(id.ok(), "enqueue: {}", id.error().to_string());
    if (i % 100 == 99) {
      clock.advance(1);
      engine.tick();
    }
  }
  const std::vector<std::string_view> queues(names.begin(), names.end());
  for (size_t i = 0; i < jobs / 20; ++i) {
    const auto lease = engine.reserve(queues, 3'600'000);
    BATON_CHECK(lease.ok() && lease->has_value(), "reserve");
  }

  std::vector<double> pauses;
  for (int i = 0; i < kPauseSamples; ++i) {
    const Stopwatch watch;
    const StateImage image = state.capture_image();
    pauses.push_back(watch.ms());
    BATON_CHECK(image.jobs.size() == jobs, "image is incomplete");
  }
  std::ranges::sort(pauses);

  const std::string dir = fresh_dir(root, "snapshot-cost");
  const StateImage image = state.capture_image();
  SnapshotInfo info;
  const double write_ms = median_ms([&] {
    const auto written = write_snapshot(fs, dir, image, engine.last_lsn(), clock.wall_now());
    BATON_CHECK(written.ok(), "write_snapshot: {}", written.error().to_string());
    info = *written;
    return 0;
  });

  const double load_ms = median_ms([&] {
    auto loaded = load_snapshot(fs, join_path(dir, snapshot_file_name(info.lsn)), StateOptions{});
    BATON_CHECK(loaded.ok(), "load_snapshot: {}", loaded.error().to_string());
    loaded->state->set_now(clock.wall_now(), clock.mono_now());
    loaded->state->end_replay(0);
    BATON_CHECK(loaded->state->job_count() == jobs, "loaded state is incomplete");
    return loaded;
  });

  std::cout << std::format(
                   "| {:>9} | {:>8.2f} | {:>8.2f} | {:>8.1f} | {:>8.1f} | {:>8.1f} | {:>8.1f} |\n",
                   jobs, pauses[pauses.size() / 2], pauses.back(), write_ms, load_ms,
                   static_cast<double>(info.bytes) / 1e6,
                   static_cast<double>(state.memory_bytes()) / 1e6)
            << std::flush;
  std::filesystem::remove_all(dir);
}

// --- recovery time ------------------------------------------------------------------------

struct History {
  const char* name;
  // Fake milliseconds between jobs, as a fraction: `advance_ms` every `per_jobs`.
  DurationMs advance_ms;
  size_t per_jobs;
};

void measure_recovery(PosixFs& fs, const std::string& root, size_t jobs, size_t payload,
                      const History& history) {
  const std::string dir = fresh_dir(root, "recovery");
  FakeClock clock;
  StateImage image;
  Lsn last_lsn = 0;
  {
    const auto recovered = recover_log(fs, dir, {}, [](const LogRecordView&) { return Status{}; });
    BATON_CHECK(recovered.ok(), "recover_log: {}", recovered.error().to_string());
    auto writer = LogWriter::open(fs,
                                  LogWriterOptions{.dir = dir,
                                                   .fsync_policy = FsyncPolicy::kInterval,
                                                   .fsync_interval_ms = 100,
                                                   .segment_size = uint64_t{64} << 20U,
                                                   .on_commit = {}},
                                  *recovered);
    BATON_CHECK(writer.ok(), "LogWriter::open: {}", writer.error().to_string());
    WriterSink sink(**writer);
    State state;
    Engine engine(state, sink, clock, EngineOptions{}, /*rng_seed=*/1);

    const std::vector<std::string> names = queue_names();
    const std::vector<std::string_view> queues(names.begin(), names.end());
    const std::string body(payload, 'x');
    for (size_t i = 0; i < jobs; ++i) {
      const auto id = engine.enqueue({.queue = names[i % kQueues], .payload = body});
      BATON_CHECK(id.ok(), "enqueue: {}", id.error().to_string());
      const auto lease = engine.reserve(queues, 60'000);
      BATON_CHECK(lease.ok() && lease->has_value(), "reserve");
      const Status acked = engine.ack((*lease)->id, (*lease)->token);
      BATON_CHECK(acked.ok(), "ack: {}", acked.error().to_string());
      if (i % history.per_jobs == history.per_jobs - 1) {
        clock.advance(history.advance_ms);
        engine.tick();  // also collects finished jobs past their retention
      }
    }
    engine.tick();
    image = state.capture_image();
    last_lsn = engine.last_lsn();
    (*writer)->flush();
    (*writer)->stop();
  }

  uint64_t log_bytes = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) log_bytes += entry.file_size();

  const auto timed_recovery = [&](Lsn expect_snapshot) {
    return median_ms([&] {
      auto recovered =
          recover_state(fs, dir, StateOptions{}, clock.wall_now(), clock.mono_now(), 0);
      BATON_CHECK(recovered.ok(), "recover_state: {}", recovered.error().to_string());
      BATON_CHECK(recovered->snapshot.lsn == expect_snapshot, "unexpected snapshot choice");
      BATON_CHECK(recovered->log.last_lsn == last_lsn, "recovery lost records");
      BATON_CHECK(recovered->state->job_count() == image.jobs.size(), "recovered another state");
      return recovered;
    });
  };

  const double from_log_ms = timed_recovery(0);
  const auto info = write_snapshot(fs, dir, image, last_lsn, clock.wall_now());
  BATON_CHECK(info.ok(), "write_snapshot: {}", info.error().to_string());
  const double from_snapshot_ms = timed_recovery(last_lsn);

  std::cout
      << std::format(
             "| {:<8} | {:>9} | {:>9} | {:>8.1f} | {:>9} | {:>8.1f} | {:>9.1f} | {:>9.1f} |\n",
             history.name, jobs, last_lsn, static_cast<double>(log_bytes) / 1e6, image.jobs.size(),
             static_cast<double>(info->bytes) / 1e6, from_log_ms, from_snapshot_ms)
      << std::flush;
  std::filesystem::remove_all(dir);
}

std::vector<size_t> parse_counts(std::string_view list) {
  std::vector<size_t> counts;
  while (!list.empty()) {
    const size_t comma = list.find(',');
    const std::string_view item = list.substr(0, comma);
    size_t value = 0;
    const auto [end, ec] = std::from_chars(item.data(), item.data() + item.size(), value);
    BATON_CHECK(ec == std::errc{} && end == item.data() + item.size() && value > 0,
                "bad job count '{}'", item);
    counts.push_back(value);
    list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
  }
  return counts;
}

int run(int argc, char** argv) {
  std::string root;
  std::vector<size_t> counts = {10'000, 100'000, 1'000'000};
  size_t payload = 256;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string_view flag = argv[i];
    const std::string_view value = argv[i + 1];
    if (flag == "--dir") {
      root = value;
    } else if (flag == "--jobs") {
      counts = parse_counts(value);
    } else if (flag == "--payload") {
      payload = parse_counts(value).front();
    } else {
      root.clear();
      break;
    }
  }
  if (root.empty() || argc % 2 == 0) {
    std::cerr << "usage: storage_bench --dir <directory> [--jobs n,n,...] [--payload bytes]\n";
    return 2;
  }
  set_log_level(LogLevel::kError);
  PosixFs fs;

  std::cout << std::format("Payload {} bytes per job.\n\n", payload);
  std::cout << "| live jobs | pause p50 ms | pause max ms | write ms | load ms | snapshot MB | "
               "state MB |\n|---:|---:|---:|---:|---:|---:|---:|\n";
  for (const size_t jobs : counts) measure_snapshot_cost(fs, root, jobs, payload);

  // 10 minutes of retention. "retained": the whole run fits in 10 s of fake
  // time. "churned": the run spans 100 minutes, ten retention periods.
  std::cout << "\n| history | jobs | log records | log MB | jobs in memory | snapshot MB | "
               "recover from log ms | recover from snapshot ms |\n"
               "|---|---:|---:|---:|---:|---:|---:|---:|\n";
  for (const size_t jobs : counts) {
    measure_recovery(fs, root, jobs, payload,
                     History{"retained", 1, std::max<size_t>(jobs / 10'000, 1)});
    measure_recovery(fs, root, jobs, payload,
                     History{"churned", 600, std::max<size_t>(jobs / 10'000, 1)});
  }
  return 0;
}

}  // namespace
}  // namespace baton

int main(int argc, char** argv) {
  try {
    return baton::run(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "storage_bench: " << e.what() << "\n";
    return 1;
  }
}

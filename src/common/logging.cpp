#include "common/logging.h"

#include <unistd.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace baton {
namespace {

// Process-wide logger state. A function-local static sidesteps static
// initialization order problems: it is constructed on first use.
struct LoggerState {
  std::atomic<LogLevel> level{LogLevel::kInfo};
  // Guards `sink` and serializes writes so lines from the event loop and the
  // log thread never interleave.
  std::mutex mutex;
  LogSink sink;
};

LoggerState& logger() {
  static LoggerState state;
  return state;
}

std::string_view level_name(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug:
      return "DEBUG";
    case LogLevel::kInfo:
      return "INFO ";
    case LogLevel::kWarn:
      return "WARN ";
    case LogLevel::kError:
      return "ERROR";
    case LogLevel::kOff:
      break;
  }
  return "?????";
}

std::string timestamp_now() {
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  tm utc{};
  gmtime_r(&ts.tv_sec, &utc);
  std::array<char, 32> buf{};
  const int n = std::snprintf(buf.data(), buf.size(), "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
                              utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
                              utc.tm_min, utc.tm_sec, ts.tv_nsec / 1'000'000);
  return {buf.data(), n > 0 ? static_cast<size_t>(n) : 0};
}

void write_stderr(std::string_view line) {
  // One write() per line keeps lines intact even if another process shares stderr.
  const char* p = line.data();
  size_t left = line.size();
  while (left > 0) {
    const ssize_t n = ::write(STDERR_FILENO, p, left);
    if (n <= 0) return;  // nothing sensible to do if stderr is gone
    p += n;
    left -= static_cast<size_t>(n);
  }
}

}  // namespace

std::optional<LogLevel> parse_log_level(std::string_view text) {
  if (text == "debug") return LogLevel::kDebug;
  if (text == "info") return LogLevel::kInfo;
  if (text == "warn") return LogLevel::kWarn;
  if (text == "error") return LogLevel::kError;
  if (text == "off") return LogLevel::kOff;
  return std::nullopt;
}

void set_log_level(LogLevel level) { logger().level.store(level, std::memory_order_relaxed); }

bool log_enabled(LogLevel level) {
  return level != LogLevel::kOff && level >= logger().level.load(std::memory_order_relaxed);
}

void set_log_sink(LogSink sink) {
  LoggerState& state = logger();
  const std::scoped_lock lock(state.mutex);
  state.sink = std::move(sink);
}

void log_message(LogLevel level, std::string_view component, std::string_view message) {
  const std::string line =
      std::format("{} {} {}: {}\n", timestamp_now(), level_name(level), component, message);
  LoggerState& state = logger();
  const std::scoped_lock lock(state.mutex);
  if (state.sink) {
    state.sink(line);
  } else {
    write_stderr(line);
  }
}

}  // namespace baton

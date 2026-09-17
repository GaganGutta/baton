#pragma once

// Diagnostic logging (not to be confused with the durable log in src/log).
//
// One line per event on stderr:
//   2026-09-17T08:15:30.123Z INFO  server: listening addr=127.0.0.1:7379
//
// Process supervisors (docker, systemd) already collect, rotate and ship stderr,
// so baton does not manage log files. Hot paths never log at INFO or above.
// See docs/design.md ("Logging") for the rationale.

#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace baton {

enum class LogLevel : uint8_t { kDebug, kInfo, kWarn, kError, kOff };

std::optional<LogLevel> parse_log_level(std::string_view text);

void set_log_level(LogLevel level);
bool log_enabled(LogLevel level);

// Replaces the destination of log lines. Pass nullptr to restore stderr.
// Intended for tests that assert on log output.
using LogSink = std::function<void(std::string_view line)>;
void set_log_sink(LogSink sink);

// Formats the line and hands it to the sink. Thread-safe.
void log_message(LogLevel level, std::string_view component, std::string_view message);

}  // namespace baton

#define BATON_LOG(level, component, ...)                                  \
  do {                                                                    \
    if (::baton::log_enabled(level)) {                                    \
      ::baton::log_message(level, component, ::std::format(__VA_ARGS__)); \
    }                                                                     \
  } while (false)

#define BATON_DEBUG(component, ...) BATON_LOG(::baton::LogLevel::kDebug, component, __VA_ARGS__)
#define BATON_INFO(component, ...) BATON_LOG(::baton::LogLevel::kInfo, component, __VA_ARGS__)
#define BATON_WARN(component, ...) BATON_LOG(::baton::LogLevel::kWarn, component, __VA_ARGS__)
#define BATON_ERROR(component, ...) BATON_LOG(::baton::LogLevel::kError, component, __VA_ARGS__)

#pragma once

// Server configuration and command-line parsing.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "common/clock.h"
#include "common/logging.h"
#include "common/result.h"
#include "log/log_writer.h"
#include "state/engine.h"
#include "state/state.h"

namespace baton {

struct ServerConfig {
  std::string dir = "./baton-data";
  std::string bind = "127.0.0.1";  // localhost only unless told otherwise
  uint16_t port = 7379;
  std::string requirepass;  // empty: no authentication

  FsyncPolicy fsync = FsyncPolicy::kAlways;
  DurationMs fsync_interval_ms = 100;
  uint64_t segment_size = uint64_t{64} << 20U;
  // Take a snapshot (and compact the log) after this many bytes of log. 0: only
  // when asked to with the SNAPSHOT command.
  uint64_t snapshot_every_bytes = uint64_t{256} << 20U;

  size_t max_connections = 10'000;
  uint64_t max_log_backlog_bytes = uint64_t{64} << 20U;  // stop reading clients beyond this
  size_t max_output_buffer_bytes = size_t{64} << 20U;    // close clients that do not read
  DurationMs max_reserve_timeout_ms = 3'600'000;

  EngineOptions engine;  // includes lease_grace_ms (--lease-grace)
  StateOptions state;
  LogLevel log_level = LogLevel::kInfo;
};

enum class CliAction : uint8_t { kRun, kShowHelp, kShowVersion };

struct ParsedArgs {
  CliAction action = CliAction::kRun;
  ServerConfig config;
};

// Parses argv[1..]. BATON_REQUIREPASS in the environment sets the password
// unless --requirepass is given (the environment keeps it out of `ps`).
Result<ParsedArgs> parse_args(std::span<const std::string_view> args);

std::string_view usage_text();

// "64m", "1g", "512k", "1048576" -> bytes. "250ms", "30s", "5m", "2h", "7d" -> ms.
Result<uint64_t> parse_size(std::string_view text);
Result<DurationMs> parse_duration_ms(std::string_view text);

}  // namespace baton

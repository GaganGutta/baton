#include "server/config.h"

#include <charconv>
#include <cstdlib>
#include <format>
#include <limits>

namespace baton {
namespace {

Error bad(std::string what) { return Error{ErrorCode::kInvalidArgument, std::move(what)}; }

// Splits "<digits><suffix>" and parses the digits.
Result<uint64_t> parse_number_with_suffix(std::string_view text, std::string_view& suffix) {
  uint64_t value = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end == text.data())
    return bad(std::format("'{}' is not a number", text));
  suffix = text.substr(static_cast<size_t>(end - text.data()));
  return value;
}

Result<uint64_t> scaled(uint64_t value, uint64_t factor, std::string_view text) {
  if (factor != 0 && value > std::numeric_limits<int64_t>::max() / factor) {
    return bad(std::format("'{}' is too large", text));
  }
  return value * factor;
}

}  // namespace

Result<uint64_t> parse_size(std::string_view text) {
  std::string_view suffix;
  BATON_ASSIGN_OR_RETURN(const uint64_t value, parse_number_with_suffix(text, suffix));
  if (suffix.empty() || suffix == "b") return value;
  if (suffix == "k" || suffix == "kb") return scaled(value, uint64_t{1} << 10U, text);
  if (suffix == "m" || suffix == "mb") return scaled(value, uint64_t{1} << 20U, text);
  if (suffix == "g" || suffix == "gb") return scaled(value, uint64_t{1} << 30U, text);
  return bad(std::format("'{}': unknown size suffix (use k, m or g)", text));
}

Result<DurationMs> parse_duration_ms(std::string_view text) {
  std::string_view suffix;
  BATON_ASSIGN_OR_RETURN(const uint64_t value, parse_number_with_suffix(text, suffix));
  uint64_t factor = 0;
  if (suffix.empty() || suffix == "ms") factor = 1;
  if (suffix == "s") factor = 1'000;
  if (suffix == "m") factor = 60'000;
  if (suffix == "h") factor = 3'600'000;
  if (suffix == "d") factor = 86'400'000;
  if (factor == 0)
    return bad(std::format("'{}': unknown duration suffix (use ms, s, m, h, d)", text));
  BATON_ASSIGN_OR_RETURN(const uint64_t ms, scaled(value, factor, text));
  return static_cast<DurationMs>(ms);
}

std::string_view usage_text() {
  return "baton - a durable job and workflow engine\n"
         "\n"
         "Usage: baton [options]\n"
         "\n"
         "Storage\n"
         "  --dir <path>                 data directory (default ./baton-data)\n"
         "  --fsync always|interval      when a write is acknowledged (default always)\n"
         "                                 always:   after fdatasync; survives power loss\n"
         "                                 interval: after write(); fdatasync on a timer.\n"
         "                                           Survives kill -9, may lose the last\n"
         "                                           interval on power loss\n"
         "  --fsync-interval <duration>  for --fsync interval (default 100ms)\n"
         "  --segment-size <size>        log segment size (default 64m)\n"
         "  --snapshot-every <size>      snapshot and compact after this much log (default\n"
         "                               256m; 0 = only on the SNAPSHOT command)\n"
         "\n"
         "Network\n"
         "  --bind <address>             IPv4/IPv6 literal to listen on (default 127.0.0.1)\n"
         "  --port <port>                default 7379\n"
         "  --requirepass <password>     require AUTH (or set BATON_REQUIREPASS)\n"
         "\n"
         "Limits\n"
         "  --max-connections <n>        default 10000\n"
         "  --max-payload <size>         largest job payload (default 1m)\n"
         "  --max-memory <size>          refuse ENQUEUE above this estimate (default 0 = "
         "unlimited)\n"
         "\n"
         "Jobs\n"
         "  --retain-finished <duration> how long finished jobs stay visible (default 10m)\n"
         "  --retain-dead <duration>     how long dead jobs stay in the DLQ (default 7d)\n"
         "  --idempotency-window <dur>   how long ENQUEUE KEY remembers a key (default 24h)\n"
         "  --lease-grace <duration>     minimum lease time left after a restart (default 5s)\n"
         "\n"
         "Other\n"
         "  --log-level debug|info|warn|error|off   (default info)\n"
         "  --version, --help\n"
         "\n"
         "Sizes take k/m/g suffixes; durations take ms/s/m/h/d.\n";
}

Result<ParsedArgs> parse_args(std::span<const std::string_view> args) {
  ParsedArgs parsed;
  ServerConfig& config = parsed.config;
  bool password_given = false;

  for (size_t i = 0; i < args.size(); ++i) {
    const std::string_view flag = args[i];
    if (flag == "--help" || flag == "-h") {
      parsed.action = CliAction::kShowHelp;
      return parsed;
    }
    if (flag == "--version") {
      parsed.action = CliAction::kShowVersion;
      return parsed;
    }
    if (i + 1 >= args.size()) return bad(std::format("{} needs a value (try --help)", flag));
    const std::string_view value = args[++i];

    if (flag == "--dir") {
      config.dir = std::string(value);
    } else if (flag == "--bind") {
      config.bind = std::string(value);
    } else if (flag == "--port") {
      BATON_ASSIGN_OR_RETURN(const uint64_t port, parse_size(value));
      if (port == 0 || port > 65535) return bad("--port must be between 1 and 65535");
      config.port = static_cast<uint16_t>(port);
    } else if (flag == "--requirepass") {
      config.requirepass = std::string(value);
      password_given = true;
    } else if (flag == "--fsync") {
      if (value == "always") {
        config.fsync = FsyncPolicy::kAlways;
      } else if (value == "interval") {
        config.fsync = FsyncPolicy::kInterval;
      } else {
        return bad("--fsync must be 'always' or 'interval'");
      }
    } else if (flag == "--fsync-interval") {
      BATON_ASSIGN_OR_RETURN(config.fsync_interval_ms, parse_duration_ms(value));
      if (config.fsync_interval_ms <= 0) return bad("--fsync-interval must be positive");
    } else if (flag == "--segment-size") {
      BATON_ASSIGN_OR_RETURN(config.segment_size, parse_size(value));
      if (config.segment_size < (uint64_t{1} << 16U)) return bad("--segment-size must be >= 64k");
    } else if (flag == "--snapshot-every") {
      BATON_ASSIGN_OR_RETURN(config.snapshot_every_bytes, parse_size(value));
    } else if (flag == "--max-connections") {
      BATON_ASSIGN_OR_RETURN(const uint64_t n, parse_size(value));
      if (n == 0) return bad("--max-connections must be positive");
      config.max_connections = static_cast<size_t>(n);
    } else if (flag == "--max-payload") {
      BATON_ASSIGN_OR_RETURN(const uint64_t n, parse_size(value));
      if (n == 0 || n > (uint64_t{64} << 20U)) return bad("--max-payload must be 1 byte to 64m");
      config.engine.max_payload_bytes = static_cast<size_t>(n);
    } else if (flag == "--max-memory") {
      BATON_ASSIGN_OR_RETURN(config.engine.max_memory_bytes, parse_size(value));
    } else if (flag == "--retain-finished") {
      BATON_ASSIGN_OR_RETURN(config.state.retain_finished_ms, parse_duration_ms(value));
    } else if (flag == "--retain-dead") {
      BATON_ASSIGN_OR_RETURN(config.state.retain_dead_ms, parse_duration_ms(value));
    } else if (flag == "--idempotency-window") {
      BATON_ASSIGN_OR_RETURN(config.engine.idempotency_window_ms, parse_duration_ms(value));
    } else if (flag == "--lease-grace") {
      BATON_ASSIGN_OR_RETURN(config.engine.lease_grace_ms, parse_duration_ms(value));
    } else if (flag == "--log-level") {
      const auto level = parse_log_level(value);
      if (!level) return bad("--log-level must be debug, info, warn, error or off");
      config.log_level = *level;
    } else {
      return bad(std::format("unknown option '{}' (try --help)", flag));
    }
  }

  if (!password_given) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded startup
    if (const char* env = std::getenv("BATON_REQUIREPASS")) config.requirepass = env;
  }
  return parsed;
}

}  // namespace baton

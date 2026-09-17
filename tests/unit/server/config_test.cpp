#include "server/config.h"

#include <gtest/gtest.h>

#include <string_view>
#include <vector>

namespace baton {
namespace {

Result<ParsedArgs> parse(const std::vector<std::string_view>& args) { return parse_args(args); }

TEST(ConfigTest, DefaultsAreSafe) {
  const auto parsed = parse({});
  ASSERT_TRUE(parsed.ok());
  EXPECT_EQ(parsed->action, CliAction::kRun);
  EXPECT_EQ(parsed->config.bind, "127.0.0.1") << "never listen on the network by default";
  EXPECT_EQ(parsed->config.port, 7379);
  EXPECT_EQ(parsed->config.fsync, FsyncPolicy::kAlways) << "durable by default";
  EXPECT_EQ(parsed->config.engine.max_payload_bytes, 1U << 20U);
}

TEST(ConfigTest, ParsesEveryOption) {
  const auto parsed = parse({"--dir",
                             "/var/lib/baton",
                             "--bind",
                             "0.0.0.0",
                             "--port",
                             "9000",
                             "--requirepass",
                             "pw",
                             "--fsync",
                             "interval",
                             "--fsync-interval",
                             "250ms",
                             "--segment-size",
                             "16m",
                             "--max-connections",
                             "50",
                             "--max-payload",
                             "64k",
                             "--max-memory",
                             "2g",
                             "--retain-finished",
                             "1h",
                             "--retain-dead",
                             "30d",
                             "--idempotency-window",
                             "2d",
                             "--lease-grace",
                             "10s",
                             "--log-level",
                             "warn"});
  ASSERT_TRUE(parsed.ok()) << parsed.error().to_string();
  const ServerConfig& c = parsed->config;
  EXPECT_EQ(c.dir, "/var/lib/baton");
  EXPECT_EQ(c.bind, "0.0.0.0");
  EXPECT_EQ(c.port, 9000);
  EXPECT_EQ(c.requirepass, "pw");
  EXPECT_EQ(c.fsync, FsyncPolicy::kInterval);
  EXPECT_EQ(c.fsync_interval_ms, 250);
  EXPECT_EQ(c.segment_size, 16U << 20U);
  EXPECT_EQ(c.max_connections, 50U);
  EXPECT_EQ(c.engine.max_payload_bytes, 64U << 10U);
  EXPECT_EQ(c.engine.max_memory_bytes, uint64_t{2} << 30U);
  EXPECT_EQ(c.state.retain_finished_ms, 3'600'000);
  EXPECT_EQ(c.state.retain_dead_ms, DurationMs{30} * 86'400'000);
  EXPECT_EQ(c.engine.idempotency_window_ms, DurationMs{2} * 86'400'000);
  EXPECT_EQ(c.engine.lease_grace_ms, 10'000);
  EXPECT_EQ(c.log_level, LogLevel::kWarn);
}

TEST(ConfigTest, HelpAndVersionShortCircuit) {
  EXPECT_EQ(parse({"--help"})->action, CliAction::kShowHelp);
  EXPECT_EQ(parse({"--port", "1", "--version"})->action, CliAction::kShowVersion);
  EXPECT_NE(usage_text().find("--fsync always|interval"), std::string_view::npos);
}

TEST(ConfigTest, RejectsNonsenseWithAUsefulMessage) {
  const std::vector<std::vector<std::string_view>> bad = {
      {"--port"},
      {"--port", "0"},
      {"--port", "70000"},
      {"--port", "http"},
      {"--fsync", "sometimes"},
      {"--fsync-interval", "0"},
      {"--max-payload", "1t"},
      {"--max-payload", "0"},
      {"--max-payload", "1g"},
      {"--segment-size", "10"},
      {"--retain-dead", "1y"},
      {"--log-level", "chatty"},
      {"--frobnicate", "1"},
      {"--max-memory", "99999999999999999999"},
  };
  for (const auto& args : bad) {
    const auto parsed = parse(args);
    ASSERT_FALSE(parsed.ok()) << args[0];
    EXPECT_EQ(parsed.error().code(), ErrorCode::kInvalidArgument) << args[0];
  }
}

TEST(ConfigTest, SizesAndDurations) {
  EXPECT_EQ(parse_size("0").value(), 0U);
  EXPECT_EQ(parse_size("512").value(), 512U);
  EXPECT_EQ(parse_size("4k").value(), 4096U);
  EXPECT_EQ(parse_size("3mb").value(), 3U << 20U);
  EXPECT_EQ(parse_size("1g").value(), 1U << 30U);
  EXPECT_FALSE(parse_size("").ok());
  EXPECT_FALSE(parse_size("-1").ok());
  EXPECT_FALSE(parse_size("1.5m").ok());

  EXPECT_EQ(parse_duration_ms("750").value(), 750);
  EXPECT_EQ(parse_duration_ms("750ms").value(), 750);
  EXPECT_EQ(parse_duration_ms("2s").value(), 2'000);
  EXPECT_EQ(parse_duration_ms("5m").value(), 300'000);
  EXPECT_EQ(parse_duration_ms("1h").value(), 3'600'000);
  EXPECT_EQ(parse_duration_ms("7d").value(), 604'800'000);
  EXPECT_FALSE(parse_duration_ms("soon").ok());
  EXPECT_FALSE(parse_duration_ms("5w").ok());
}

}  // namespace
}  // namespace baton

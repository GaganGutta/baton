#include "common/logging.h"

#include <gtest/gtest.h>

#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace baton {
namespace {

// Captures log lines for the duration of a test and restores defaults after.
class LoggingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    set_log_level(LogLevel::kDebug);
    set_log_sink([this](std::string_view line) { lines_.emplace_back(line); });
  }
  void TearDown() override {
    set_log_sink(nullptr);
    set_log_level(LogLevel::kInfo);
  }

  std::vector<std::string> lines_;
};

TEST_F(LoggingTest, FormatsOneLinePerMessage) {
  BATON_INFO("server", "listening addr={} port={}", "127.0.0.1", 7379);
  ASSERT_EQ(lines_.size(), 1U);
  const std::regex expected(
      R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z INFO  server: listening addr=127\.0\.0\.1 port=7379\n$)");
  EXPECT_TRUE(std::regex_match(lines_[0], expected)) << lines_[0];
}

TEST_F(LoggingTest, LevelFiltersMessages) {
  set_log_level(LogLevel::kWarn);
  BATON_DEBUG("t", "debug");
  BATON_INFO("t", "info");
  BATON_WARN("t", "warn");
  BATON_ERROR("t", "error");
  ASSERT_EQ(lines_.size(), 2U);
  EXPECT_NE(lines_[0].find("WARN  t: warn"), std::string::npos);
  EXPECT_NE(lines_[1].find("ERROR t: error"), std::string::npos);
}

TEST_F(LoggingTest, OffSilencesEverything) {
  set_log_level(LogLevel::kOff);
  BATON_ERROR("t", "error");
  EXPECT_TRUE(lines_.empty());
}

TEST_F(LoggingTest, ArgumentsAreNotEvaluatedWhenFiltered) {
  set_log_level(LogLevel::kError);
  int evaluations = 0;
  BATON_DEBUG("t", "{}", ++evaluations);
  EXPECT_EQ(evaluations, 0);
}

TEST_F(LoggingTest, ConcurrentWritersProduceWholeLines) {
  constexpr int kThreads = 4;
  constexpr int kPerThread = 200;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([t] {
      for (int i = 0; i < kPerThread; ++i) BATON_INFO("worker", "thread={} i={}", t, i);
    });
  }
  for (auto& th : threads) th.join();
  ASSERT_EQ(lines_.size(), static_cast<size_t>(kThreads * kPerThread));
  for (const auto& line : lines_) {
    EXPECT_EQ(line.back(), '\n');
    EXPECT_NE(line.find("worker: thread="), std::string::npos);
  }
}

TEST(LogLevelTest, ParsesKnownNames) {
  EXPECT_EQ(parse_log_level("debug"), LogLevel::kDebug);
  EXPECT_EQ(parse_log_level("info"), LogLevel::kInfo);
  EXPECT_EQ(parse_log_level("warn"), LogLevel::kWarn);
  EXPECT_EQ(parse_log_level("error"), LogLevel::kError);
  EXPECT_EQ(parse_log_level("off"), LogLevel::kOff);
  EXPECT_EQ(parse_log_level("verbose"), std::nullopt);
  EXPECT_EQ(parse_log_level(""), std::nullopt);
}

}  // namespace
}  // namespace baton

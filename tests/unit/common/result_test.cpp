#include "common/result.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <memory>
#include <string>

namespace baton {
namespace {

Result<int> parse_positive(int v) {
  if (v <= 0) return Error{ErrorCode::kInvalidArgument, "must be positive"};
  return v;
}

Status require_even(int v) {
  if (v % 2 != 0) return Error{ErrorCode::kFailedPrecondition, "odd"};
  return {};
}

Result<int> doubled_if_valid(int v) {
  BATON_ASSIGN_OR_RETURN(const int parsed, parse_positive(v));
  BATON_RETURN_IF_ERROR(require_even(parsed));
  return parsed * 2;
}

TEST(ResultTest, HoldsValue) {
  const Result<int> r = parse_positive(7);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.value(), 7);
  EXPECT_EQ(*r, 7);
}

TEST(ResultTest, HoldsError) {
  const Result<int> r = parse_positive(-1);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code(), ErrorCode::kInvalidArgument);
  EXPECT_EQ(r.error().message(), "must be positive");
  EXPECT_EQ(r.error().to_string(), "invalid argument: must be positive");
}

TEST(ResultTest, MoveOnlyValuesWork) {
  Result<std::unique_ptr<std::string>> r = std::make_unique<std::string>("payload");
  ASSERT_TRUE(r.ok());
  const std::unique_ptr<std::string> taken = std::move(r).value();
  EXPECT_EQ(*taken, "payload");
}

TEST(ResultTest, ArrowOperatorReachesValue) {
  Result<std::string> r = std::string("abc");
  EXPECT_EQ(r->size(), 3U);
}

TEST(ResultTest, StatusDefaultsToOk) {
  const Status s;
  EXPECT_TRUE(s.ok());
  EXPECT_TRUE(require_even(4).ok());
  EXPECT_FALSE(require_even(3).ok());
}

TEST(ResultTest, MacrosPropagateErrors) {
  EXPECT_EQ(doubled_if_valid(4).value(), 8);
  EXPECT_EQ(doubled_if_valid(-4).error().code(), ErrorCode::kInvalidArgument);
  EXPECT_EQ(doubled_if_valid(3).error().code(), ErrorCode::kFailedPrecondition);
}

TEST(ResultTest, IoErrorIncludesErrnoText) {
  const Error e = io_error("open /nope", ENOENT);
  EXPECT_EQ(e.code(), ErrorCode::kIo);
  EXPECT_NE(e.message().find("open /nope"), std::string::npos);
  EXPECT_NE(e.message().find("No such file"), std::string::npos);
}

TEST(ResultTest, EveryErrorCodeHasAName) {
  for (int i = 0; i <= static_cast<int>(ErrorCode::kInternal); ++i) {
    EXPECT_NE(to_string(static_cast<ErrorCode>(i)), "unknown") << "code " << i;
  }
}

using ResultDeathTest = ::testing::Test;

TEST(ResultDeathTest, ValueOnErrorAborts) {
  const Result<int> r = parse_positive(-1);
  EXPECT_DEATH({ (void)r.value(); }, "Result::value\\(\\) on error: invalid argument");
}

TEST(ResultDeathTest, ErrorOnSuccessAborts) {
  const Result<int> r = parse_positive(1);
  EXPECT_DEATH({ (void)r.error(); }, "Result::error\\(\\) on success");
}

}  // namespace
}  // namespace baton

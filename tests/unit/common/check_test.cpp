#include "common/check.h"

#include <gtest/gtest.h>

namespace baton {
namespace {

TEST(CheckTest, PassingCheckDoesNothing) {
  int evaluations = 0;
  BATON_CHECK(++evaluations == 1);
  BATON_CHECK(evaluations == 1, "value was {}", evaluations);
  EXPECT_EQ(evaluations, 1);  // the condition is evaluated exactly once
}

TEST(CheckDeathTest, FailingCheckAbortsWithLocationAndExpression) {
  // GoogleTest uses POSIX extended regular expressions here, so no \d.
  EXPECT_DEATH(BATON_CHECK(1 + 1 == 3), "check_test.cpp:[0-9]+: check failed: 1 \\+ 1 == 3");
}

TEST(CheckDeathTest, FailingCheckIncludesFormattedMessage) {
  const int lsn = 42;
  EXPECT_DEATH(BATON_CHECK(lsn == 0, "unexpected lsn {}", lsn), "unexpected lsn 42");
}

TEST(CheckDeathTest, UnreachableAborts) {
  EXPECT_DEATH(BATON_UNREACHABLE("state {}", 9), "unreachable: state 9");
}

}  // namespace
}  // namespace baton

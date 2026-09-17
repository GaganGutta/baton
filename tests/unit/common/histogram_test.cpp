#include "common/histogram.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace baton {
namespace {

TEST(HistogramTest, EmptyIsAllZero) {
  const Histogram h;
  EXPECT_EQ(h.count(), 0U);
  EXPECT_EQ(h.min(), 0U);
  EXPECT_EQ(h.max(), 0U);
  EXPECT_EQ(h.mean(), 0.0);
  EXPECT_EQ(h.percentile(0.99), 0U);
}

TEST(HistogramTest, BucketBoundaries) {
  EXPECT_EQ(Histogram::bucket_for(0), 0U);
  EXPECT_EQ(Histogram::bucket_for(1), 1U);
  EXPECT_EQ(Histogram::bucket_for(2), 2U);
  EXPECT_EQ(Histogram::bucket_for(3), 2U);
  EXPECT_EQ(Histogram::bucket_for(4), 3U);
  EXPECT_EQ(Histogram::bucket_for(UINT64_MAX), 64U);
  // Every value is <= the upper bound of its bucket and > the previous bound.
  const std::vector<uint64_t> samples = {1, 2, 3, 4, 1000, uint64_t{1} << 40U, UINT64_MAX};
  for (const uint64_t v : samples) {
    const size_t b = Histogram::bucket_for(v);
    EXPECT_LE(v, Histogram::bucket_upper_bound(b));
    EXPECT_GT(v, Histogram::bucket_upper_bound(b - 1));
  }
}

TEST(HistogramTest, TracksExactCountSumMinMaxMean) {
  Histogram h;
  for (const uint64_t v : {5ULL, 1ULL, 9ULL, 5ULL}) h.record(v);
  EXPECT_EQ(h.count(), 4U);
  EXPECT_EQ(h.sum(), 20U);
  EXPECT_EQ(h.min(), 1U);
  EXPECT_EQ(h.max(), 9U);
  EXPECT_DOUBLE_EQ(h.mean(), 5.0);
}

TEST(HistogramTest, PercentilesAreBucketUpperBoundsClampedToMax) {
  Histogram h;
  for (int i = 0; i < 99; ++i) h.record(10);  // bucket [8, 15]
  h.record(1000);                             // bucket [512, 1023]
  EXPECT_EQ(h.percentile(0.50), 15U);
  EXPECT_EQ(h.percentile(0.99), 15U);
  EXPECT_EQ(h.percentile(1.0), 1000U);  // clamped to the exact max, not 1023
}

TEST(HistogramTest, PercentileNeverUnderestimates) {
  Histogram h;
  for (uint64_t v = 1; v <= 1000; ++v) h.record(v);
  EXPECT_GE(h.percentile(0.5), 500U);
  EXPECT_LE(h.percentile(0.5), 1000U);
  EXPECT_GE(h.percentile(0.999), 999U);
}

}  // namespace
}  // namespace baton

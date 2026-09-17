// The histogram's percentiles end up in INFO, in the metrics and - through the
// load generator - in the README, so its accuracy is tested against sorted arrays.

#include "common/histogram.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <random>
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

TEST(HistogramTest, TracksExactCountSumMinMaxMean) {
  Histogram h;
  for (const uint64_t v : {5ULL, 1ULL, 9ULL, 5ULL}) h.record(v);
  EXPECT_EQ(h.count(), 4U);
  EXPECT_EQ(h.sum(), 20U);
  EXPECT_EQ(h.min(), 1U);
  EXPECT_EQ(h.max(), 9U);
  EXPECT_DOUBLE_EQ(h.mean(), 5.0);
}

TEST(HistogramTest, SmallValuesAreExactAndTheMaximumAlwaysIs) {
  Histogram h;
  for (int i = 0; i < 99; ++i) h.record(10);
  h.record(1000);
  EXPECT_EQ(h.percentile(0.50), 10U);
  EXPECT_EQ(h.percentile(0.99), 10U);
  EXPECT_EQ(h.percentile(1.0), 1000U) << "clamped to the exact maximum, not a slot boundary";
}

TEST(HistogramTest, EveryValueLandsInASlotAtMostThreePercentWide) {
  std::mt19937_64 rng(7);
  std::vector<uint64_t> values = {0, 1, 31, 32, 33, 63, 64, 65, 1'000, 1'000'000, UINT64_MAX};
  for (int i = 0; i < 100'000; ++i) values.push_back(rng() >> (rng() % 64));
  for (const uint64_t v : values) {
    const size_t slot = Histogram::slot_for(v);
    ASSERT_LT(slot, Histogram::kSlots);
    const uint64_t upper = Histogram::upper_bound_of(slot);
    ASSERT_GE(upper, v) << v;
    ASSERT_LE(upper - v, v / 32) << "slot too wide for " << v;
    if (slot > 0) {
      ASSERT_LT(Histogram::upper_bound_of(slot - 1), v) << v;
    }
  }
}

TEST(HistogramTest, SlotsAreContiguousAndCoverEverything) {
  uint64_t previous = 0;
  for (size_t slot = 1; slot < Histogram::kSlots; ++slot) {
    const uint64_t upper = Histogram::upper_bound_of(slot);
    ASSERT_GT(upper, previous);
    ASSERT_EQ(Histogram::slot_for(previous + 1), slot) << "gap before slot " << slot;
    previous = upper;
  }
  EXPECT_EQ(previous, UINT64_MAX);
}

TEST(HistogramTest, PercentilesMatchASortedArrayWithinTheSlotError) {
  std::mt19937_64 rng(11);
  std::lognormal_distribution<double> latency(12.0, 1.5);  // around 160 us, with a long tail
  std::vector<uint64_t> values;
  Histogram h;
  for (int i = 0; i < 200'000; ++i) {
    const auto v = static_cast<uint64_t>(latency(rng));
    values.push_back(v);
    h.record(v);
  }
  std::ranges::sort(values);
  for (const double q : {0.5, 0.9, 0.99, 0.999, 1.0}) {
    const size_t rank = std::max<size_t>(1, static_cast<size_t>((q * 200'000) + 0.999999));
    const uint64_t exact = values[rank - 1];
    const uint64_t reported = h.percentile(q);
    EXPECT_GE(reported, exact) << "never underestimates; q=" << q;
    EXPECT_LE(static_cast<double>(reported), (static_cast<double>(exact) * (1.0 + 1.0 / 32)) + 1)
        << q;
  }
  EXPECT_EQ(h.percentile(1.0), values.back());
  EXPECT_EQ(h.min(), values.front());
}

TEST(HistogramTest, MergingEqualsRecordingEverythingInOne) {
  Histogram a;
  Histogram b;
  Histogram all;
  Histogram merged;
  for (uint64_t v = 1; v < 50'000; v += 7) {
    (v % 2 == 0 ? a : b).record(v * 13);
    all.record(v * 13);
  }
  merged.merge(a);
  merged.merge(b);
  merged.merge(Histogram{});
  EXPECT_EQ(merged.count(), all.count());
  EXPECT_EQ(merged.sum(), all.sum());
  EXPECT_EQ(merged.min(), all.min());
  EXPECT_EQ(merged.max(), all.max());
  for (const double q : {0.01, 0.5, 0.99, 0.9999}) {
    EXPECT_EQ(merged.percentile(q), all.percentile(q));
  }
}

}  // namespace
}  // namespace baton

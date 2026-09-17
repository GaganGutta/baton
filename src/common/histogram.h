#pragma once

// A fixed-size log-linear histogram, in the spirit of HdrHistogram: every power
// of two is split into 32 equal sub-buckets, so a reported percentile is at most
// 1/32 (about 3%) above the true value over the whole uint64_t range, in 1,920
// counters. Values below 32 are exact; count, sum, min, max and mean are always
// exact. Recording is a bit scan and an increment.
//
// Used for group-commit batch sizes and fsync latency (INFO, metrics) and by the
// load generator, whose percentiles end up in the README - which is why there is
// one implementation, and why it is tested against sorted arrays.
//
// Not thread-safe: the owner provides synchronization, or keeps one per thread
// and merges them.

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace baton {

class Histogram {
 public:
  static constexpr unsigned kSubBucketBits = 5;
  static constexpr uint64_t kSubBuckets = uint64_t{1} << kSubBucketBits;
  static constexpr size_t kSlots = (64 - kSubBucketBits + 1) * kSubBuckets;

  void record(uint64_t value) {
    ++counts_[slot_for(value)];
    ++count_;
    sum_ += value;
    min_ = count_ == 1 ? value : std::min(min_, value);
    max_ = std::max(max_, value);
  }

  void merge(const Histogram& other) {
    if (other.count_ == 0) return;
    for (size_t i = 0; i < kSlots; ++i) counts_[i] += other.counts_[i];
    min_ = count_ == 0 ? other.min_ : std::min(min_, other.min_);
    max_ = std::max(max_, other.max_);
    count_ += other.count_;
    sum_ += other.sum_;
  }

  uint64_t count() const { return count_; }
  uint64_t sum() const { return sum_; }
  uint64_t min() const { return count_ == 0 ? 0 : min_; }
  uint64_t max() const { return max_; }
  double mean() const {
    return count_ == 0 ? 0.0 : static_cast<double>(sum_) / static_cast<double>(count_);
  }

  // The upper bound of the slot that holds the q-quantile (0 < q <= 1): never
  // below the true value, at most 1/32 above it, and never above the exact
  // maximum. 0 when empty.
  uint64_t percentile(double q) const {
    if (count_ == 0) return 0;
    const auto wanted = static_cast<uint64_t>((q * static_cast<double>(count_)) + 0.999999);
    const uint64_t target = std::clamp<uint64_t>(wanted, 1, count_);
    uint64_t seen = 0;
    for (size_t i = 0; i < kSlots; ++i) {
      seen += counts_[i];
      if (seen >= target) return std::min(upper_bound_of(i), max_);
    }
    return max_;
  }

  static size_t slot_for(uint64_t value) {
    if (value < kSubBuckets) return static_cast<size_t>(value);
    const unsigned exponent = static_cast<unsigned>(std::bit_width(value)) - 1;  // >= 5
    const unsigned shift = exponent - kSubBucketBits;
    const uint64_t sub = (value >> shift) & (kSubBuckets - 1);
    return static_cast<size_t>(((exponent - kSubBucketBits + 1) * kSubBuckets) + sub);
  }

  // The largest value that lands in `slot`.
  static uint64_t upper_bound_of(size_t slot) {
    if (slot < kSubBuckets) return slot;
    const unsigned exponent = static_cast<unsigned>(slot / kSubBuckets) + kSubBucketBits - 1;
    const uint64_t sub = slot % kSubBuckets;
    const unsigned shift = exponent - kSubBucketBits;
    const uint64_t lower = (kSubBuckets + sub) << shift;
    return lower + ((uint64_t{1} << shift) - 1);
  }

 private:
  std::array<uint64_t, kSlots> counts_{};
  uint64_t count_ = 0;
  uint64_t sum_ = 0;
  uint64_t min_ = 0;
  uint64_t max_ = 0;
};

}  // namespace baton

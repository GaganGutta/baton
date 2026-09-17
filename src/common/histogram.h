#pragma once

// A fixed-size histogram with power-of-two buckets, used for group-commit batch
// sizes, fsync latency and (later) metrics and the load generator.
//
// Bucket i counts values v with 2^(i-1) <= v < 2^i (bucket 0 holds v == 0), so
// 65 buckets cover the whole uint64_t range and recording is a bit scan.
// Percentiles are reported as the upper bound of the bucket they fall in, i.e.
// they over-estimate by at most 2x; exact min/max/mean are tracked separately.
// Not thread-safe: the owner provides synchronization.

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>

namespace baton {

class Histogram {
 public:
  static constexpr size_t kBuckets = 65;

  void record(uint64_t value) {
    ++buckets_[bucket_for(value)];
    ++count_;
    sum_ += value;
    min_ = count_ == 1 ? value : std::min(min_, value);
    max_ = std::max(max_, value);
  }

  uint64_t count() const { return count_; }
  uint64_t sum() const { return sum_; }
  uint64_t min() const { return count_ == 0 ? 0 : min_; }
  uint64_t max() const { return max_; }
  double mean() const {
    return count_ == 0 ? 0.0 : static_cast<double>(sum_) / static_cast<double>(count_);
  }

  // Upper bound of the bucket containing the q-quantile (0 < q <= 1), clamped
  // to the exact maximum. Returns 0 when empty.
  uint64_t percentile(double q) const {
    if (count_ == 0) return 0;
    const auto target = static_cast<uint64_t>(q * static_cast<double>(count_));
    uint64_t seen = 0;
    for (size_t i = 0; i < kBuckets; ++i) {
      seen += buckets_[i];
      if (seen >= std::max<uint64_t>(target, 1)) return std::min(bucket_upper_bound(i), max_);
    }
    return max_;
  }

  const std::array<uint64_t, kBuckets>& buckets() const { return buckets_; }

  // Largest value that lands in bucket i.
  static uint64_t bucket_upper_bound(size_t i) {
    if (i == 0) return 0;
    if (i >= 64) return UINT64_MAX;
    return (uint64_t{1} << i) - 1;
  }

  static size_t bucket_for(uint64_t value) { return static_cast<size_t>(std::bit_width(value)); }

 private:
  std::array<uint64_t, kBuckets> buckets_{};
  uint64_t count_ = 0;
  uint64_t sum_ = 0;
  uint64_t min_ = 0;
  uint64_t max_ = 0;
};

}  // namespace baton

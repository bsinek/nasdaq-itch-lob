#pragma once
#include <cstdint>
#include <vector>

namespace itch {

// Fixed-bin nanosecond histogram: 1 ns bins to 16 µs, then one overflow bin.
// Percentiles are exact at bin resolution; the true limiting resolution is
// the timer tick, which the benchmark measures and reports separately.
class Hist {
 public:
  static constexpr uint64_t kMaxNs = 16384;
  Hist() : bins_(kMaxNs, 0) {}

  void add(uint64_t ns) {
    ++n_;
    sum_ += ns;
    if (ns > max_) max_ = ns;
    if (ns < kMaxNs) ++bins_[ns]; else ++overflow_;
  }

  uint64_t pct(double p) const {
    const uint64_t target = uint64_t(p * double(n_));
    uint64_t acc = 0;
    for (uint64_t i = 0; i < kMaxNs; ++i) {
      acc += bins_[i];
      if (acc >= target) return i;
    }
    return max_;
  }

  uint64_t n() const { return n_; }
  uint64_t overflow() const { return overflow_; }
  uint64_t max() const { return max_; }
  double mean() const { return n_ ? double(sum_) / double(n_) : 0.0; }

 private:
  std::vector<uint64_t> bins_;
  uint64_t overflow_ = 0, n_ = 0, max_ = 0, sum_ = 0;
};

}  // namespace itch

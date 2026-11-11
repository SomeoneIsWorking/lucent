#include "lucent/count_histogram.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace lucent::perf {

std::size_t CountHistogram::bucket_of(std::uint64_t value) {
  if (value < sub) {
    return static_cast<std::size_t>(value);
  }
  const unsigned shift = static_cast<unsigned>(std::bit_width(value)) - 1U - sub_bits;
  return sub + (shift * sub) + static_cast<std::size_t>((value >> shift) - sub);
}

std::uint64_t CountHistogram::bucket_top(std::size_t bucket) {
  if (bucket < sub) {
    return bucket;
  }
  const std::size_t shift = (bucket - sub) / sub;
  const std::uint64_t mantissa = sub + ((bucket - sub) % sub);
  return ((mantissa + 1U) << shift) - 1U;
}

void CountHistogram::add(std::uint64_t value) {
  ++buckets_[bucket_of(value)];
  ++count_;
  total_ += value;
  max_ = std::max(max_, value);
}

std::uint64_t CountHistogram::percentile(double fraction) const {
  if (count_ == 0) {
    return 0;
  }
  const auto rank = std::max<std::uint64_t>(
      1, static_cast<std::uint64_t>(std::ceil(fraction * static_cast<double>(count_))));
  std::uint64_t seen = 0;
  for (std::size_t bucket = 0; bucket < buckets; ++bucket) {
    seen += buckets_[bucket];
    if (seen >= rank) {
      return std::min(bucket_top(bucket), max_);
    }
  }
  return max_;
}

} // namespace lucent::perf

// A constant-memory distribution of work counts (instructions, draws, pixels) for percentiles.
// Exact below 64; above, 64 buckets per power of two, so a percentile is at most 1.6% above the
// true value. The maximum and the total are exact.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace lucent::perf {

class CountHistogram {
public:
  void add(std::uint64_t value);

  [[nodiscard]] std::uint64_t count() const {
    return count_;
  }
  [[nodiscard]] std::uint64_t total() const {
    return total_;
  }
  [[nodiscard]] std::uint64_t max() const {
    return max_;
  }
  // The top of the bucket `fraction` of the samples fall at or below, never above max(); 0 when
  // empty.
  [[nodiscard]] std::uint64_t percentile(double fraction) const;

private:
  static constexpr unsigned sub_bits = 6;
  static constexpr std::size_t sub = std::size_t{1} << sub_bits;
  static constexpr std::size_t buckets = sub + ((64 - sub_bits) * sub);
  static std::size_t bucket_of(std::uint64_t value);
  static std::uint64_t bucket_top(std::size_t bucket);

  std::array<std::uint32_t, buckets> buckets_{};
  std::uint64_t count_ = 0;
  std::uint64_t total_ = 0;
  std::uint64_t max_ = 0;
};

} // namespace lucent::perf

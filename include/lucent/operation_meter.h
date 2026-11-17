// Named per-operation cost on a WorkClock: each operation's work per call, self (nested operations
// excluded) and inclusive, per frame, and its calls per frame, as the `perf-units` run-end lines.
#pragma once

#include "lucent/count_histogram.h"
#include "lucent/work_clock.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lucent::perf {

class OperationMeter {
public:
  using Operation = std::uint32_t;

  // Meters while `clock` is on, on the thread the clock is running.
  explicit OperationMeter(const WorkClock &clock) : clock_(clock) {
  }
  OperationMeter(const OperationMeter &) = delete;
  OperationMeter &operator=(const OperationMeter &) = delete;
  OperationMeter(OperationMeter &&) = delete;
  OperationMeter &operator=(OperationMeter &&) = delete;

  // The id of `name`, registered on first use; look it up once, outside the hot path.
  Operation operation(std::string_view name);

  // An operation for the extent of a scope. While the clock is off, one branch at each end.
  class Scope {
  public:
    Scope(OperationMeter &meter, Operation operation) : meter_(meter) {
      if (meter_.clock_.on()) {
        depth_ = meter_.enter(operation);
      }
    }
    ~Scope() {
      if (depth_ != 0) {
        meter_.exit(depth_);
      }
    }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
    Scope(Scope &&) = delete;
    Scope &operator=(Scope &&) = delete;

  private:
    OperationMeter &meter_;
    std::size_t depth_ = 0;
  };

  // A frame ended: its calls count when `counted`, else they are dropped. No scope may be open.
  void end_frame(bool counted = true);

  struct Totals {
    std::uint64_t calls = 0;
    std::uint64_t self = 0;
    std::uint64_t inclusive = 0;
  };
  // Over the counted frames.
  [[nodiscard]] Totals totals(Operation operation) const;

  // The run-end lines on the `perf` channel: a header, then per operation self, inclusive, per
  // frame and calls.
  void report() const;

private:
  static constexpr std::size_t max_depth = 64;

  struct Call {
    std::uint64_t self = 0;
    std::uint64_t inclusive = 0;
  };
  struct Record {
    std::string name;
    CountHistogram self;
    CountHistogram inclusive;
    CountHistogram per_frame;
    CountHistogram calls;
    std::vector<Call> this_frame;
    std::uint64_t frame_self = 0;
  };
  struct Open {
    Operation operation = 0;
    std::uint64_t start = 0;
    std::uint64_t child = 0;
  };

  std::size_t enter(Operation operation);
  void exit(std::size_t depth);

  const WorkClock &clock_;
  std::vector<Record> records_;
  std::array<Open, max_depth> open_{};
  std::size_t depth_ = 0;
  std::uint64_t frames_ = 0;
};

} // namespace lucent::perf

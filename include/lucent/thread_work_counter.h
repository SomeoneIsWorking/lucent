// The calling thread's own user-space work, in a unit host load does not move: instructions
// retired, or the thread's CPU time where no instruction counter opens.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace lucent::perf {

enum class WorkUnit : std::uint8_t {
  instructions,    // user-space instructions retired (Linux perf_event)
  cpu_nanoseconds, // the thread's CPU time, where no instruction counter opens
};

// A counter of the thread that constructs it. read() is called on that thread only.
class ThreadWorkCounter {
public:
  // The instruction counter when this host opens one, else thread CPU time.
  ThreadWorkCounter();
  // `unit` only; valid() is false when this host cannot count it for this thread.
  explicit ThreadWorkCounter(WorkUnit unit);
  ~ThreadWorkCounter();
  ThreadWorkCounter(const ThreadWorkCounter &) = delete;
  ThreadWorkCounter &operator=(const ThreadWorkCounter &) = delete;
  ThreadWorkCounter(ThreadWorkCounter &&) = delete;
  ThreadWorkCounter &operator=(ThreadWorkCounter &&) = delete;

  [[nodiscard]] bool valid() const {
    return valid_;
  }
  [[nodiscard]] WorkUnit unit() const {
    return unit_;
  }
  [[nodiscard]] std::uint64_t read() const;

private:
  bool open_instructions();
  void close_all();

#if defined(__linux__)
  // A hybrid CPU counts each core type on its own PMU; their sum is the thread's.
  static constexpr std::size_t max_pmus = 2;
  std::array<int, max_pmus> fds_{-1, -1};
#endif
  WorkUnit unit_ = WorkUnit::instructions;
  bool valid_ = false;
};

} // namespace lucent::perf

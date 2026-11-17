#include <lucent/count_histogram.h>
#include <lucent/operation_meter.h>
#include <lucent/thread_work_counter.h>
#include <lucent/work_clock.h>
#include "test_environment.h"

#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

// Work the optimizer cannot remove.
std::uint64_t spin(std::uint64_t rounds) {
  volatile std::uint64_t sum = 0;
  for (std::uint64_t i = 0; i < rounds; ++i) {
    sum = sum + i;
  }
  return sum;
}

int histogram_failures() {
  int failures = 0;
  lucent::perf::CountHistogram empty;
  if (empty.percentile(0.5) != 0 || empty.count() != 0) {
    std::cerr << "an empty histogram reported a sample\n";
    ++failures;
  }
  lucent::perf::CountHistogram small;
  for (std::uint64_t value = 1; value <= 50; ++value) {
    small.add(value);
  }
  if (small.percentile(0.5) != 25 || small.percentile(1.0) != 50 || small.total() != 1275) {
    std::cerr << "a histogram below 64 was not exact\n";
    ++failures;
  }
  lucent::perf::CountHistogram large;
  for (std::uint64_t value = 1; value <= 100000; ++value) {
    large.add(value * 1000);
  }
  const std::uint64_t p99 = large.percentile(0.99);
  if (p99 < 99'000'000 || p99 > 99'000'000 + (99'000'000 / 62) || large.max() != 100'000'000) {
    std::cerr << "a large histogram's p99 " << p99 << " left its 1.6% bound\n";
    ++failures;
  }
  return failures;
}

int counter_failures() {
  int failures = 0;
  const lucent::perf::ThreadWorkCounter counter;
  if (!counter.valid()) {
    std::cerr << "the default counter did not open\n";
    return 1;
  }
  const std::uint64_t before = counter.read();
  spin(2'000'000);
  if (counter.read() <= before) {
    std::cerr << "the counter did not advance over work\n";
    ++failures;
  }
  return failures;
}

// The fiber thread's work is on the clock, and the starting thread's work while the fiber runs is
// not.
int clock_failures() {
  lucent::perf::WorkClock clock;
  lucent::perf::WorkClock::Thread fiber_place;
  std::mutex mutex;
  std::condition_variable turn_changed;
  bool fiber_turn = false;
  bool done = false;

  clock.start();
  std::thread fiber([&] {
    std::unique_lock<std::mutex> lock(mutex);
    turn_changed.wait(lock, [&] {
      return fiber_turn;
    });
    clock.enter(&fiber_place);
    spin(4'000'000);
    clock.leave();
    done = true;
    fiber_turn = false;
    turn_changed.notify_all();
  });
  spin(1'000'000);
  const std::uint64_t before_handoff = clock.now();
  lucent::perf::WorkClock::Thread *const self = clock.leave();
  {
    std::unique_lock<std::mutex> lock(mutex);
    fiber_turn = true;
    turn_changed.notify_all();
    turn_changed.wait(lock, [&] {
      return done;
    });
  }
  clock.enter(self);
  fiber.join();
  const std::uint64_t fiber_work = clock.now() - before_handoff;
  clock.stop();
  if (clock.uncounted_threads() != 0 || fiber_work <= before_handoff) {
    std::cerr << "the clock missed the fiber's work: " << fiber_work << " after " << before_handoff
              << "\n";
    return 1;
  }
  return 0;
}

// Nested operations: the outer's self excludes the inner's work, its inclusive holds both, and an
// uncounted frame's calls are dropped.
int operation_failures() {
  lucent::perf::WorkClock clock;
  lucent::perf::OperationMeter meter(clock);
  const auto outer = meter.operation("outer");
  const auto inner = meter.operation("inner");
  {
    const lucent::perf::OperationMeter::Scope off(meter, outer);
    spin(1000);
  }
  meter.end_frame();
  clock.start();
  for (int frame = 0; frame < 4; ++frame) {
    {
      const lucent::perf::OperationMeter::Scope scope(meter, outer);
      spin(1'000'000);
      for (int call = 0; call < 2; ++call) {
        const lucent::perf::OperationMeter::Scope nested(meter, inner);
        spin(3'000'000);
      }
    }
    meter.end_frame(frame != 0);
  }
  clock.stop();
  const auto o = meter.totals(outer);
  const auto i = meter.totals(inner);
  int failures = 0;
  if (o.calls != 3 || i.calls != 6) {
    std::cerr << "operation calls " << o.calls << " and " << i.calls << ", not 3 and 6\n";
    ++failures;
  }
  if (i.self < 5 * o.self || i.self > 7 * o.self) {
    std::cerr << "inner self " << i.self << " is not six outer selves " << o.self << "\n";
    ++failures;
  }
  if (o.inclusive != o.self + i.inclusive) {
    std::cerr << "outer inclusive " << o.inclusive << " is not its self plus inner's\n";
    ++failures;
  }
  meter.report();
  return failures;
}

} // namespace

int main() {
  return lucent::test::run_main([] {
    const int failures =
        histogram_failures() + counter_failures() + clock_failures() + operation_failures();
    if (failures == 0) {
      std::cout << "perf tests passed\n";
    }
    return failures == 0 ? 0 : 1;
  });
}

#include "lucent/thread_work_counter.h"

#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <fstream>
#include <string>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <ctime>
#endif

namespace lucent::perf {
namespace {

#if defined(__linux__)
// The PMU type of a hybrid CPU's core kind, or 0 when this CPU has no such PMU.
std::uint64_t hybrid_pmu_type(const char *pmu) {
  std::ifstream file(std::string("/sys/bus/event_source/devices/") + pmu + "/type");
  std::uint64_t type = 0;
  file >> type;
  return file ? type : 0;
}

int open_instruction_counter(std::uint64_t pmu_type) {
  perf_event_attr attr{};
  attr.size = sizeof(attr);
  attr.type = PERF_TYPE_HARDWARE;
  // A hybrid PMU is named in the config's high word (PERF_PMU_TYPE_SHIFT).
  attr.config = (pmu_type << 32U) | PERF_COUNT_HW_INSTRUCTIONS;
  attr.exclude_kernel = 1;
  attr.exclude_hv = 1;
  // pid 0, cpu -1: this thread, on whichever CPU it runs.
  return static_cast<int>(syscall(SYS_perf_event_open, &attr, 0, -1, -1, PERF_FLAG_FD_CLOEXEC));
}
#endif

std::uint64_t thread_cpu_nanoseconds() {
#if defined(_WIN32)
  FILETIME created{};
  FILETIME exited{};
  FILETIME kernel{};
  FILETIME user{};
  GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
  const auto ticks = [](const FILETIME &time) {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
  };
  return (ticks(kernel) + ticks(user)) * 100U; // 100 ns units
#else
  timespec now{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
  return (static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000U) +
         static_cast<std::uint64_t>(now.tv_nsec);
#endif
}

} // namespace

ThreadWorkCounter::ThreadWorkCounter() {
  valid_ = open_instructions();
  if (!valid_) {
    unit_ = WorkUnit::cpu_nanoseconds;
    valid_ = true;
  }
}

ThreadWorkCounter::ThreadWorkCounter(WorkUnit unit) : unit_(unit) {
  valid_ = unit == WorkUnit::cpu_nanoseconds || open_instructions();
}

ThreadWorkCounter::~ThreadWorkCounter() {
  close_all();
}

bool ThreadWorkCounter::open_instructions() {
#if defined(__linux__)
  std::size_t opened = 0;
  for (const char *pmu : {"cpu_core", "cpu_atom"}) {
    const std::uint64_t type = hybrid_pmu_type(pmu);
    if (type != 0) {
      fds_[opened++] = open_instruction_counter(type);
    }
  }
  if (opened == 0) {
    fds_[0] = open_instruction_counter(0);
    opened = 1;
  }
  for (std::size_t i = 0; i < opened; ++i) {
    if (fds_[i] < 0) {
      close_all();
      return false;
    }
  }
  return true;
#else
  return false;
#endif
}

void ThreadWorkCounter::close_all() {
#if defined(__linux__)
  for (int &fd : fds_) {
    if (fd >= 0) {
      close(fd);
    }
    fd = -1;
  }
#endif
}

std::uint64_t ThreadWorkCounter::read() const {
  if (!valid_) {
    return 0;
  }
  if (unit_ == WorkUnit::cpu_nanoseconds) {
    return thread_cpu_nanoseconds();
  }
  std::uint64_t total = 0;
#if defined(__linux__)
  for (const int fd : fds_) {
    std::uint64_t value = 0;
    if (fd >= 0 && ::read(fd, &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value))) {
      total += value;
    }
  }
#endif
  return total;
}

} // namespace lucent::perf

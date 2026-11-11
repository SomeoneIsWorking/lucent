// A worker stops when destroyed or replaced, its sleep wakes on the stop instead of running out,
// and a default token never stops.
#include "lucent/worker.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

int main() {
  using namespace std::chrono_literals;
  const lucent::StopToken never;
  assert(!never.stop_requested() && !never.sleep_for(1ms));

  std::atomic<bool> woke_early{false};
  const auto started = std::chrono::steady_clock::now();
  {
    lucent::Worker worker{[&woke_early](const lucent::StopToken &stop) {
      woke_early = stop.sleep_for(60s) && stop.stop_requested();
    }};
    std::this_thread::sleep_for(20ms);
  }
  assert(woke_early && std::chrono::steady_clock::now() - started < 30s);

  std::atomic<int> stopped{0};
  const auto count_stop = [&stopped](const lucent::StopToken &stop) {
    while (!stop.sleep_for(10ms)) {
    }
    ++stopped;
  };
  lucent::Worker replaced{count_stop};
  replaced = lucent::Worker{count_stop};
  assert(stopped == 1 && replaced.joinable());
  replaced.request_stop();
  replaced.join();
  assert(stopped == 2);
  std::puts("workers stop when destroyed or replaced and wake from sleep on the stop");
  return 0;
}

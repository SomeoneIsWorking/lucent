// A process that waits on StopSignals wakes for SIGTERM and SIGINT sent to it while another thread
// runs, and names which it was.
#include "lucent/stop_signals.h"

#include <cassert>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>

#include <unistd.h>

int main() {
  lucent::StopSignals stop;
  // Signals sent from another thread are still taken by `wait`, not by a default handler.
  std::thread sender{[] {
    kill(getpid(), SIGTERM);
  }};
  sender.join();
  const std::string first = stop.wait();
  assert(first == "SIGTERM");
  kill(getpid(), SIGINT);
  const std::string second = stop.wait();
  assert(second == "SIGINT");
  std::puts("stop signals wake the waiter and name the signal");
  return 0;
}

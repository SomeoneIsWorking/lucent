// lucent/stop_signals.h — what a server process waits for before it shuts down: SIGINT and SIGTERM
// on POSIX; Ctrl+C, Ctrl+Break, closing the console, logoff and shutdown on Windows.
#pragma once

#include <string>

#ifndef _WIN32
#include <csignal>
#endif

namespace lucent {

// Construct it before starting any thread, so on POSIX only `wait` takes the signals. One per
// process.
class StopSignals {
public:
  StopSignals();
  ~StopSignals();
  StopSignals(const StopSignals &) = delete;
  StopSignals &operator=(const StopSignals &) = delete;
  StopSignals(StopSignals &&) = delete;
  StopSignals &operator=(StopSignals &&) = delete;

  // Blocks until a stop request arrives; what it was, for the log.
  std::string wait();

private:
#ifndef _WIN32
  sigset_t signals_{};
#endif
};

} // namespace lucent

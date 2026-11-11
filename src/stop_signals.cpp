#include "lucent/stop_signals.h"

#include <stdexcept>

#ifdef _WIN32
#include <atomic>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace lucent {

#ifdef _WIN32
namespace {

// The console control handler takes no context, so the event it sets lives here.
std::atomic<HANDLE> g_stop_event{nullptr};
std::atomic<DWORD> g_stop_kind{0};

BOOL WINAPI on_console_event(DWORD kind) {
  g_stop_kind.store(kind);
  SetEvent(g_stop_event.load());
  // Handled: the process ends when `wait`'s caller returns from main.
  return TRUE;
}

std::string name_of(DWORD kind) {
  switch (kind) {
  case CTRL_C_EVENT:
    return "Ctrl+C";
  case CTRL_BREAK_EVENT:
    return "Ctrl+Break";
  case CTRL_CLOSE_EVENT:
    return "console closed";
  case CTRL_LOGOFF_EVENT:
    return "logoff";
  case CTRL_SHUTDOWN_EVENT:
    return "shutdown";
  default:
    return "console event " + std::to_string(kind);
  }
}

} // namespace

StopSignals::StopSignals() {
  HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE expected = nullptr;
  if (event == nullptr || !g_stop_event.compare_exchange_strong(expected, event)) {
    if (event != nullptr) {
      CloseHandle(event);
    }
    throw std::runtime_error{"stop signals are already taken in this process"};
  }
  if (SetConsoleCtrlHandler(on_console_event, TRUE) == 0) {
    g_stop_event.store(nullptr);
    CloseHandle(event);
    throw std::runtime_error{"no console control handler"};
  }
}

StopSignals::~StopSignals() {
  SetConsoleCtrlHandler(on_console_event, FALSE);
  CloseHandle(g_stop_event.exchange(nullptr));
}

std::string StopSignals::wait() {
  WaitForSingleObject(g_stop_event.load(), INFINITE);
  return name_of(g_stop_kind.load());
}

#else

StopSignals::StopSignals() {
  sigemptyset(&signals_);
  sigaddset(&signals_, SIGINT);
  sigaddset(&signals_, SIGTERM);
  if (pthread_sigmask(SIG_BLOCK, &signals_, nullptr) != 0) {
    throw std::runtime_error{"cannot block SIGINT and SIGTERM"};
  }
}

StopSignals::~StopSignals() {
  pthread_sigmask(SIG_UNBLOCK, &signals_, nullptr);
}

std::string StopSignals::wait() {
  int received = 0;
  sigwait(&signals_, &received);
  return received == SIGINT ? "SIGINT" : "SIGTERM";
}

#endif

} // namespace lucent

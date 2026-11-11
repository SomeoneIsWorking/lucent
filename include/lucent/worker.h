// lucent/worker.h — a thread that is asked to stop and joined when it goes, in place of
// std::jthread and std::stop_token, which Android NDK 28's libc++ still keeps experimental.
#pragma once

#include <chrono>
#include <concepts>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace lucent {

namespace detail {

struct StopState {
  std::mutex mutex;
  std::condition_variable wake;
  bool stop = false;
};

} // namespace detail

// Whether the owner asked a worker to stop. A default token never asks.
class StopToken {
public:
  StopToken() = default;
  explicit StopToken(detail::StopState &state) noexcept : state_{&state} {
  }

  [[nodiscard]] bool stop_requested() const {
    if (state_ == nullptr) {
      return false;
    }
    const std::lock_guard lock{state_->mutex};
    return state_->stop;
  }
  // Sleeps up to `wait`, waking early when asked to stop; true when asked.
  template <typename Rep, typename Period>
  bool sleep_for(std::chrono::duration<Rep, Period> wait) const {
    if (state_ == nullptr) {
      std::this_thread::sleep_for(wait);
      return false;
    }
    std::unique_lock lock{state_->mutex};
    return state_->wake.wait_for(lock, wait, [this] {
      return state_->stop;
    });
  }

private:
  detail::StopState *state_ = nullptr;
};

// One thread running `body(token)`; destroying or replacing it asks it to stop and joins it.
class Worker {
public:
  Worker() = default;
  template <typename Body>
    requires std::invocable<Body &, const StopToken &>
  explicit Worker(Body body) : state_{std::make_unique<detail::StopState>()} {
    thread_ = std::thread{[body = std::move(body), token = StopToken{*state_}]() mutable {
      body(token);
    }};
  }
  ~Worker() {
    finish();
  }
  Worker(const Worker &) = delete;
  Worker &operator=(const Worker &) = delete;
  Worker(Worker &&) noexcept = default;
  Worker &operator=(Worker &&other) noexcept {
    if (this != &other) {
      finish();
      state_ = std::move(other.state_);
      thread_ = std::move(other.thread_);
    }
    return *this;
  }

  void request_stop() {
    if (state_) {
      {
        const std::lock_guard lock{state_->mutex};
        state_->stop = true;
      }
      state_->wake.notify_all();
    }
  }
  [[nodiscard]] bool joinable() const noexcept {
    return thread_.joinable();
  }
  void join() {
    thread_.join();
  }

private:
  void finish() {
    request_stop();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  // On the heap, so the running thread's token survives a move of its Worker.
  std::unique_ptr<detail::StopState> state_;
  std::thread thread_;
};

} // namespace lucent

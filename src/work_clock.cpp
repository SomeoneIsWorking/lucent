#include "lucent/work_clock.h"

#include "lucent/log.h"

#include <cstdlib>

namespace lucent::perf {

void WorkClock::start() {
  if (on_) {
    return;
  }
  const std::thread::id self = std::this_thread::get_id();
  if (starting_.counter_ && self != starter_) {
    log(Level::Error, "perf", "a work clock was started on a thread other than the one it counts");
    std::abort();
  }
  starter_ = self;
  on_ = true;
  open(starting_);
  running_ = &starting_;
  starting_.since_ = starting_.read();
}

void WorkClock::stop() {
  on_ = false;
  running_ = nullptr;
}

std::uint64_t WorkClock::now() const {
  if (running_ == nullptr) {
    return banked_;
  }
  return banked_ + running_->read() - running_->since_;
}

WorkClock::Thread *WorkClock::leave() {
  if (!on_ || running_ == nullptr) {
    return nullptr;
  }
  Thread *left = running_;
  banked_ += left->read() - left->since_;
  running_ = nullptr;
  return left;
}

void WorkClock::enter(Thread *place) {
  if (!on_ || place == nullptr) {
    return;
  }
  open(*place);
  running_ = place;
  place->since_ = place->read();
}

void WorkClock::open(Thread &place) {
  if (place.counter_) {
    return;
  }
  if (unit_) {
    place.counter_.emplace(*unit_);
  } else {
    place.counter_.emplace();
    unit_ = place.counter_->unit();
  }
  if (!place.counter_->valid()) {
    ++uncounted_;
    log(Level::Error, "perf",
        "a thread's work counter did not open; its work is missing from the clock");
  }
}

} // namespace lucent::perf

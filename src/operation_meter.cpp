#include "lucent/operation_meter.h"

#include "lucent/log.h"

#include <cstdlib>

namespace lucent::perf {

OperationMeter::Operation OperationMeter::operation(std::string_view name) {
  for (std::size_t i = 0; i < records_.size(); ++i) {
    if (records_[i].name == name) {
      return static_cast<Operation>(i);
    }
  }
  Record &record = records_.emplace_back();
  record.name = name;
  // Frames counted before it existed had none of its calls.
  for (std::uint64_t frame = 0; frame < frames_; ++frame) {
    record.per_frame.add(0);
    record.calls.add(0);
  }
  return static_cast<Operation>(records_.size() - 1);
}

std::size_t OperationMeter::enter(Operation operation) {
  if (depth_ == max_depth) {
    log(Level::Error, "perf", "operations nest deeper than 64");
    std::abort();
  }
  Open &open = open_[depth_++];
  open.operation = operation;
  open.child = 0;
  open.start = clock_.now();
  return depth_;
}

void OperationMeter::exit(std::size_t depth) {
  const std::uint64_t now = clock_.now();
  if (depth != depth_) {
    log(Level::Error, "perf", "an operation closed while one opened inside it is still open");
    std::abort();
  }
  const Open &open = open_[--depth_];
  const std::uint64_t inclusive = now - open.start;
  Record &record = records_[open.operation];
  record.this_frame.push_back({inclusive - open.child, inclusive});
  record.frame_self += inclusive - open.child;
  if (depth_ > 0) {
    open_[depth_ - 1].child += inclusive;
  }
}

void OperationMeter::end_frame(bool counted) {
  if (depth_ != 0) {
    log(Level::Error, "perf", "an operation is open at the end of a frame");
    std::abort();
  }
  if (counted) {
    ++frames_;
  }
  for (Record &record : records_) {
    if (counted) {
      for (const Call &call : record.this_frame) {
        record.self.add(call.self);
        record.inclusive.add(call.inclusive);
      }
      record.per_frame.add(record.frame_self);
      record.calls.add(record.this_frame.size());
    }
    record.this_frame.clear();
    record.frame_self = 0;
  }
}

OperationMeter::Totals OperationMeter::totals(Operation operation) const {
  const Record &record = records_.at(operation);
  return {record.calls.total(), record.self.total(), record.inclusive.total()};
}

void OperationMeter::report() const {
  if (records_.empty()) {
    return;
  }
  const bool instructions = clock_.unit() == WorkUnit::instructions;
  const char *unit = instructions ? "insn" : "cpu-ns";
  info("perf", "ops run-end: {} frames, {}; self excludes nested operations", frames_,
       instructions ? "user instructions retired"
                    : "thread CPU time ns (fallback: no instruction counter opens here; moves "
                      "with load and clock speed)");
  const auto line = [&](const std::string &name, const char *what, const CountHistogram &work,
                        const char *work_unit, const char *per) {
    info("perf", "op {} {}: p50 {} p99 {} max {} {}/{}, total {} {}", name, what,
         work.percentile(0.5), work.percentile(0.99), work.max(), work_unit, per, work.total(),
         work_unit);
  };
  for (const Record &record : records_) {
    line(record.name, "self", record.self, unit, "call");
    line(record.name, "inclusive", record.inclusive, unit, "call");
    line(record.name, "per frame", record.per_frame, unit, "frame");
    line(record.name, "calls", record.calls, "calls", "frame");
  }
}

} // namespace lucent::perf

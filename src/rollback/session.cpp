#include "lucent/rollback/session.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace lucent::rollback {
namespace {

constexpr std::uint8_t kVersion = 1;
// Inputs per slot in one message; a link further behind catches up over several.
constexpr std::int64_t kMaxRunFrames = 64;
constexpr std::int64_t kKeptHashes = 256;
constexpr int kMaxInputDelay = 30;
constexpr int kMaxRollback = 60;
constexpr int kMaxLinks = 16;

void put_u32(std::string &out, std::uint64_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<char>((value >> static_cast<unsigned>(shift)) & 0xFFU));
  }
}

void put_u64(std::string &out, std::uint64_t value) {
  put_u32(out, value >> 32U);
  put_u32(out, value & 0xFFFFFFFFU);
}

// Reads a message front to back; once short, every read fails.
class Reader {
public:
  explicit Reader(std::string_view bytes) : bytes_{bytes} {
  }
  std::optional<std::uint8_t> u8() {
    if (at_ + 1 > bytes_.size()) {
      return std::nullopt;
    }
    return static_cast<std::uint8_t>(bytes_[at_++]);
  }
  std::optional<std::uint32_t> u32() {
    if (at_ + 4 > bytes_.size()) {
      return std::nullopt;
    }
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
      value = (value << 8U) | static_cast<std::uint8_t>(bytes_[at_++]);
    }
    return value;
  }
  std::optional<std::uint64_t> u64() {
    const std::optional<std::uint32_t> high = u32();
    const std::optional<std::uint32_t> low = u32();
    if (!high || !low) {
      return std::nullopt;
    }
    return (static_cast<std::uint64_t>(*high) << 32U) | *low;
  }
  std::optional<std::string_view> bytes(std::size_t count) {
    if (at_ + count > bytes_.size()) {
      return std::nullopt;
    }
    const std::string_view out = bytes_.substr(at_, count);
    at_ += count;
    return out;
  }
  bool done() const noexcept {
    return at_ == bytes_.size();
  }

private:
  std::string_view bytes_;
  std::size_t at_ = 0;
};

void validate(const Config &config, int link_count) {
  const bool sizes = config.slots >= 1 && config.slots <= kMaxSlots && config.input_bytes >= 1 &&
                     config.input_bytes <= kMaxInputBytes && config.input_delay >= 0 &&
                     config.input_delay <= kMaxInputDelay && config.max_rollback >= 1 &&
                     config.max_rollback <= kMaxRollback && link_count >= 0 &&
                     link_count <= kMaxLinks && !config.local_slots.empty();
  std::vector<int> seen = config.local_slots;
  std::sort(seen.begin(), seen.end());
  const bool slots = std::adjacent_find(seen.begin(), seen.end()) == seen.end() &&
                     seen.front() >= 0 && seen.back() < config.slots;
  if (!sizes || !slots) {
    throw std::invalid_argument{"a rollback config outside its limits"};
  }
}

} // namespace

Session::Session(Config config, Runtime &runtime, int link_count)
    : config_{std::move(config)}, runtime_{runtime} {
  validate(config_, link_count);
  logs_.resize(static_cast<std::size_t>(config_.slots));
  acked_.assign(static_cast<std::size_t>(link_count),
                std::vector<std::int64_t>(static_cast<std::size_t>(config_.slots), 0));
  // A local input plays `input_delay` frames late, so the frames before it play no input.
  for (const int slot : config_.local_slots) {
    for (int at = 0; at < config_.input_delay; ++at) {
      confirm(slot, Input(static_cast<std::size_t>(config_.input_bytes), 0), at);
    }
  }
}

bool Session::local(int slot) const noexcept {
  return std::find(config_.local_slots.begin(), config_.local_slots.end(), slot) !=
         config_.local_slots.end();
}

std::int64_t Session::confirmed_frame() const noexcept {
  std::int64_t lowest = std::numeric_limits<std::int64_t>::max();
  for (const SlotLog &log : logs_) {
    lowest = std::min(lowest, log.confirmed_until());
  }
  return lowest;
}

int Session::ring(std::int64_t at) const noexcept {
  return static_cast<int>(at % (config_.max_rollback + 1));
}

void Session::confirm(int slot, const Input &input, std::int64_t at) {
  SlotLog &log = logs_[static_cast<std::size_t>(slot)];
  // Only the next frame: an earlier one is known and a later one arrives again in order.
  if (at != log.confirmed_until()) {
    return;
  }
  log.inputs.push_back(input);
  const auto ran = used_.find(at);
  if (ran != used_.end() && ran->second[static_cast<std::size_t>(slot)] != input) {
    rewind_from_ = std::min(rewind_from_.value_or(at), at);
  }
}

Session::Input Session::input_for(const SlotLog &log, std::int64_t at) const {
  if (at >= log.first && at < log.confirmed_until()) {
    return log.inputs[static_cast<std::size_t>(at - log.first)];
  }
  // Predicted: the slot's newest confirmed input, as if the player held it.
  if (!log.inputs.empty()) {
    return log.inputs.back();
  }
  return Input(static_cast<std::size_t>(config_.input_bytes), 0);
}

void Session::run(std::int64_t at, bool resimulating) {
  std::vector<Input> inputs;
  std::vector<std::uint8_t> all;
  inputs.reserve(logs_.size());
  all.reserve(logs_.size() * static_cast<std::size_t>(config_.input_bytes));
  for (int slot = 0; slot < config_.slots; ++slot) {
    inputs.push_back(input_for(logs_[static_cast<std::size_t>(slot)], at));
    all.insert(all.end(), inputs.back().begin(), inputs.back().end());
  }
  used_[at] = std::move(inputs);
  runtime_.setInputs(all);
  runtime_.advance(resimulating);
  run_hashes_[at] = runtime_.hash();
}

void Session::finalize() {
  // A frame that ran and whose every input is now confirmed has run with the true inputs: any
  // input that differed from the one it used rewound it this tick.
  const std::int64_t until = std::min(confirmed_frame(), frame_);
  for (; finalized_ < until; ++finalized_) {
    our_hashes_[finalized_] = run_hashes_.at(finalized_);
    compare_hashes(finalized_);
  }
}

std::optional<std::uint64_t> Session::final_hash(std::int64_t at) const {
  const auto found = our_hashes_.find(at);
  return found != our_hashes_.end() ? std::optional<std::uint64_t>{found->second} : std::nullopt;
}

void Session::compare_hashes(std::int64_t at) {
  const auto ours = our_hashes_.find(at);
  const auto theirs = their_hashes_.find(at);
  if (ours != our_hashes_.end() && theirs != their_hashes_.end() &&
      ours->second != theirs->second && !desync_frame_) {
    desync_frame_ = at;
  }
}

Status Session::tick(std::span<const std::uint8_t> local_input) {
  if (desync_frame_) {
    return Status::Desynced;
  }
  if (local_input.size() !=
      config_.local_slots.size() * static_cast<std::size_t>(config_.input_bytes)) {
    throw std::invalid_argument{"a local input of the wrong size"};
  }
  std::int64_t remote = std::numeric_limits<std::int64_t>::max();
  for (int slot = 0; slot < config_.slots; ++slot) {
    if (!local(slot)) {
      remote = std::min(remote, logs_[static_cast<std::size_t>(slot)].confirmed_until());
    }
  }
  // The ring must still hold the earliest frame a late input could change.
  if (frame_ - config_.max_rollback > remote) {
    return Status::Stalled;
  }
  const auto bytes = static_cast<std::size_t>(config_.input_bytes);
  for (std::size_t index = 0; index < config_.local_slots.size(); ++index) {
    confirm(config_.local_slots[index],
            Input(local_input.begin() + static_cast<std::ptrdiff_t>(index * bytes),
                  local_input.begin() + static_cast<std::ptrdiff_t>((index + 1) * bytes)),
            frame_ + config_.input_delay);
  }
  if (rewind_from_ && *rewind_from_ < frame_) {
    const std::int64_t from = *rewind_from_;
    runtime_.load(ring(from));
    for (std::int64_t at = from; at < frame_; ++at) {
      if (at != from) {
        runtime_.save(ring(at));
      }
      run(at, true);
      ++resimulated_;
    }
  }
  rewind_from_.reset();
  runtime_.save(ring(frame_));
  run(frame_, false);
  ++frame_;
  finalize();
  trim();
  return desync_frame_ ? Status::Desynced : Status::Advanced;
}

void Session::receive(int link, std::string_view message) {
  if (link < 0 || link >= static_cast<int>(acked_.size())) {
    return;
  }
  Reader reader{message};
  const std::optional<std::uint8_t> version = reader.u8();
  const std::optional<std::uint8_t> slots = reader.u8();
  if (version != kVersion || slots != config_.slots) {
    return;
  }
  std::vector<std::int64_t> acks;
  for (int slot = 0; slot < config_.slots; ++slot) {
    const std::optional<std::uint32_t> ack = reader.u32();
    if (!ack) {
      return;
    }
    acks.push_back(*ack);
  }
  struct Run {
    int slot;
    std::int64_t start;
    std::string_view bytes;
  };
  std::vector<Run> runs;
  const std::optional<std::uint8_t> run_count = reader.u8();
  for (int index = 0; run_count && index < *run_count; ++index) {
    const std::optional<std::uint8_t> slot = reader.u8();
    const std::optional<std::uint32_t> start = reader.u32();
    const std::optional<std::uint8_t> count = reader.u8();
    const std::optional<std::string_view> bytes =
        count ? reader.bytes(static_cast<std::size_t>(*count) *
                             static_cast<std::size_t>(config_.input_bytes))
              : std::nullopt;
    if (!slot || !start || !bytes || *slot >= config_.slots) {
      return;
    }
    runs.push_back({*slot, *start, *bytes});
  }
  const std::optional<std::uint8_t> has_hash = reader.u8();
  std::optional<std::pair<std::int64_t, std::uint64_t>> hash;
  if (has_hash == 1) {
    const std::optional<std::uint32_t> at = reader.u32();
    const std::optional<std::uint64_t> value = reader.u64();
    if (!at || !value) {
      return;
    }
    hash = std::pair<std::int64_t, std::uint64_t>{*at, *value};
  }
  if (!run_count || !has_hash || *has_hash > 1 || !reader.done()) {
    return;
  }
  std::vector<std::int64_t> &acked = acked_[static_cast<std::size_t>(link)];
  for (std::size_t slot = 0; slot < acks.size(); ++slot) {
    acked[slot] = std::max(acked[slot], acks[slot]);
  }
  const auto width = static_cast<std::size_t>(config_.input_bytes);
  for (const Run &each : runs) {
    // A peer never speaks for this peer's own slots.
    if (local(each.slot)) {
      continue;
    }
    for (std::size_t index = 0; index * width < each.bytes.size(); ++index) {
      const std::string_view input = each.bytes.substr(index * width, width);
      confirm(each.slot, Input(input.begin(), input.end()),
              each.start + static_cast<std::int64_t>(index));
    }
  }
  if (hash) {
    their_hashes_[hash->first] = hash->second;
    compare_hashes(hash->first);
  }
}

std::string Session::message_for(int link) const {
  std::string out;
  out.push_back(static_cast<char>(kVersion));
  out.push_back(static_cast<char>(config_.slots));
  for (const SlotLog &log : logs_) {
    put_u32(out, static_cast<std::uint64_t>(log.confirmed_until()));
  }
  std::string runs;
  int run_count = 0;
  for (int slot = 0; slot < config_.slots; ++slot) {
    const SlotLog &log = logs_[static_cast<std::size_t>(slot)];
    const std::int64_t acked =
        link >= 0 && link < static_cast<int>(acked_.size())
            ? acked_[static_cast<std::size_t>(link)][static_cast<std::size_t>(slot)]
            : 0;
    const std::int64_t start = std::max(acked, log.first);
    const std::int64_t count = std::min(log.confirmed_until() - start, kMaxRunFrames);
    if (count <= 0) {
      continue;
    }
    runs.push_back(static_cast<char>(slot));
    put_u32(runs, static_cast<std::uint64_t>(start));
    runs.push_back(static_cast<char>(count));
    for (std::int64_t at = start; at < start + count; ++at) {
      const Input &input = log.inputs[static_cast<std::size_t>(at - log.first)];
      runs.append(input.begin(), input.end());
    }
    ++run_count;
  }
  out.push_back(static_cast<char>(run_count));
  out += runs;
  if (our_hashes_.empty()) {
    out.push_back(0);
  } else {
    out.push_back(1);
    put_u32(out, static_cast<std::uint64_t>(our_hashes_.rbegin()->first));
    put_u64(out, our_hashes_.rbegin()->second);
  }
  return out;
}

void Session::trim() {
  const std::int64_t rewind_floor = frame_ - config_.max_rollback - 1;
  for (int slot = 0; slot < config_.slots; ++slot) {
    SlotLog &log = logs_[static_cast<std::size_t>(slot)];
    // Kept while a link still needs it or a rewind could read it; the newest always, to predict.
    std::int64_t keep = rewind_floor;
    for (const std::vector<std::int64_t> &acked : acked_) {
      keep = std::min(keep, acked[static_cast<std::size_t>(slot)]);
    }
    while (log.first < keep && log.inputs.size() > 1) {
      log.inputs.pop_front();
      ++log.first;
    }
  }
  used_.erase(used_.begin(), used_.lower_bound(rewind_floor));
  run_hashes_.erase(run_hashes_.begin(), run_hashes_.lower_bound(finalized_));
  const std::int64_t hash_floor = frame_ - kKeptHashes;
  our_hashes_.erase(our_hashes_.begin(), our_hashes_.lower_bound(hash_floor));
  their_hashes_.erase(their_hashes_.begin(), their_hashes_.lower_bound(hash_floor));
}

} // namespace lucent::rollback

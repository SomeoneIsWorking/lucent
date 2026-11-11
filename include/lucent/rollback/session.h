// lucent/rollback/session.h — rollback netplay for a deterministic game: input exchange,
// prediction, rewind and resimulate, input delay and per-frame state-hash desync detection. The
// session knows no transport: the application carries its messages over whatever links it has.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lucent::rollback {

inline constexpr int kMaxSlots = 8;
inline constexpr int kMaxInputBytes = 8;

// What the game supplies. It owns its state storage; the session only names ring slots.
class Runtime {
public:
  Runtime() = default;
  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;
  Runtime(Runtime &&) = delete;
  Runtime &operator=(Runtime &&) = delete;
  virtual ~Runtime() = default;

  // Keeps the state at the start of this frame in ring slot `slot` (0 to max_rollback).
  virtual void save(int slot) = 0;
  virtual void load(int slot) = 0;
  // Every slot's input for the next frame, slot-major.
  virtual void setInputs(std::span<const std::uint8_t> all) = 0;
  // One logic frame; no audio or presentation while `resimulating`.
  virtual void advance(bool resimulating) = 0;
  // The state's hash, equal on every peer for the same frame.
  [[nodiscard]] virtual std::uint64_t hash() const = 0;
};

struct Config {
  int slots = 2;
  int input_bytes = 1;
  // The slots this peer's player or players drive.
  std::vector<int> local_slots;
  // A local input plays this many frames after it is given.
  int input_delay = 2;
  // How far back a late input may rewind; the game stalls rather than run further ahead.
  int max_rollback = 8;
};

enum class Status : std::uint8_t {
  // A frame ran.
  Advanced,
  // The other peers are too far behind for another frame; the local input was not taken.
  Stalled,
  // A peer's state hash differs from ours at `Session::desync_frame()`.
  Desynced,
};

// One session per peer. Each tick: `receive` what the links brought, `tick` with the local input,
// then send `message_for` each link. Links are numbered by the application, 0 to link_count - 1;
// inputs a peer learns from one link are passed on to the others, so a host relays a star.
class Session {
public:
  // Throws std::invalid_argument for a config outside the limits above.
  Session(Config config, Runtime &runtime, int link_count);

  // Runs the next frame with `local` (local slots' input bytes, in `local_slots` order), first
  // rewinding to the earliest frame a newly learned input changed.
  Status tick(std::span<const std::uint8_t> local);
  // A message from link `link`; one that does not read is ignored.
  void receive(int link, std::string_view message);
  // What link `link` has not yet acknowledged: inputs, acknowledgements and our newest final hash.
  std::string message_for(int link) const;

  // The next frame to run.
  std::int64_t frame() const noexcept {
    return frame_;
  }
  // Every frame before this has every slot's input confirmed; with input delay it can run ahead
  // of `frame()`, so a confirmed frame may not have run yet.
  std::int64_t confirmed_frame() const noexcept;
  // Every frame before this has run with every input confirmed, so `final_hash` has it (while the
  // session keeps it).
  std::int64_t finalized_frame() const noexcept {
    return finalized_;
  }
  std::optional<std::int64_t> desync_frame() const noexcept {
    return desync_frame_;
  }
  // Our state hash after frame `at` ran with every input confirmed, while the session keeps it.
  std::optional<std::uint64_t> final_hash(std::int64_t at) const;
  // Frames re-run so far, across every rewind.
  std::int64_t resimulated_frames() const noexcept {
    return resimulated_;
  }

private:
  using Input = std::vector<std::uint8_t>;

  struct SlotLog {
    // Confirmed inputs from `first` on.
    std::int64_t first = 0;
    std::deque<Input> inputs;
    std::int64_t confirmed_until() const noexcept {
      return first + static_cast<std::int64_t>(inputs.size());
    }
  };

  bool local(int slot) const noexcept;
  void confirm(int slot, const Input &input, std::int64_t at);
  Input input_for(const SlotLog &log, std::int64_t at) const;
  void run(std::int64_t at, bool resimulating);
  int ring(std::int64_t at) const noexcept;
  void finalize();
  void compare_hashes(std::int64_t at);
  void trim();

  Config config_;
  Runtime &runtime_;
  std::vector<SlotLog> logs_;
  // The inputs each frame since the oldest rewind target last ran with.
  std::map<std::int64_t, std::vector<Input>> used_;
  // Each link's confirmed frame per slot, from its acknowledgements.
  std::vector<std::vector<std::int64_t>> acked_;
  std::int64_t frame_ = 0;
  std::optional<std::int64_t> rewind_from_;
  // The hash after each frame since `finalized_` as it last ran, predicted inputs and all.
  std::map<std::int64_t, std::uint64_t> run_hashes_;
  std::int64_t finalized_ = 0;
  // Final hashes of recent frames: ours, and those the links reported.
  std::map<std::int64_t, std::uint64_t> our_hashes_;
  std::map<std::int64_t, std::uint64_t> their_hashes_;
  std::optional<std::int64_t> desync_frame_;
  std::int64_t resimulated_ = 0;
};

} // namespace lucent::rollback

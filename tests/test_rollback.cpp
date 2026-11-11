// Rollback sessions over simulated links that delay, drop and reorder: every peer's final state
// hashes match a lockstep run of the true inputs (two peers on a mesh, four on a host-relayed
// star), late inputs rewind and resimulate, a changed state is reported as a desync near its
// frame, and a peer that hears nothing stalls instead of running past its ring.
#include "lucent/rollback/session.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace {

using lucent::rollback::Config;
using lucent::rollback::Session;
using lucent::rollback::Status;

constexpr int kDelay = 2;
constexpr int kRollback = 8;
constexpr std::int64_t kFrames = 600;

// Eight fighters on a line and a random generator the inputs stir.
struct State {
  std::array<std::int32_t, 8> position{};
  std::uint32_t random = 0x12345678U;
  std::int64_t frames = 0;
};

class Game final : public lucent::rollback::Runtime {
public:
  explicit Game(int slots) : slots_{slots}, ring_(kRollback + 1) {
  }

  void save(int slot) override {
    ring_[static_cast<std::size_t>(slot)] = state_;
  }
  void load(int slot) override {
    state_ = ring_[static_cast<std::size_t>(slot)];
  }
  void setInputs(std::span<const std::uint8_t> all) override {
    inputs_.assign(all.begin(), all.end());
  }
  void advance(bool resimulating) override {
    for (int slot = 0; slot < slots_; ++slot) {
      const std::uint8_t input = inputs_[static_cast<std::size_t>(slot)];
      state_.random = state_.random * 1664525U + 1013904223U + input;
      state_.position[static_cast<std::size_t>(slot)] +=
          static_cast<std::int32_t>(input & 7U) - 3 +
          static_cast<std::int32_t>(state_.random >> 30U);
    }
    if (drift_from_ && state_.frames >= *drift_from_) {
      ++state_.position[0];
    }
    ++state_.frames;
    resimulating ? ++resimulated_ : ++presented_;
  }
  std::uint64_t hash() const override {
    std::uint64_t value = 1469598103934665603ULL;
    const auto mix = [&value](std::uint64_t part) {
      value = (value ^ part) * 1099511628211ULL;
    };
    for (const std::int32_t each : state_.position) {
      mix(static_cast<std::uint32_t>(each));
    }
    mix(state_.random);
    mix(static_cast<std::uint64_t>(state_.frames));
    return value;
  }

  // A fault a desync check must catch: from frame `frame` on, this game plays differently.
  void drift_from(std::int64_t frame) {
    drift_from_ = frame;
  }
  std::int64_t presented() const {
    return presented_;
  }

private:
  int slots_;
  State state_;
  std::vector<State> ring_;
  std::vector<std::uint8_t> inputs_;
  std::int64_t presented_ = 0;
  std::int64_t resimulated_ = 0;
  std::optional<std::int64_t> drift_from_;
};

// The input slot `slot` gives for frame `frame`, the same wherever it is asked.
std::uint8_t input_of(int slot, std::int64_t frame) {
  if (frame < kDelay) {
    return 0;
  }
  std::uint32_t value =
      static_cast<std::uint32_t>(std::int64_t{slot} * 7919 + (frame - kDelay) / 5);
  value = value * 2654435761U;
  return static_cast<std::uint8_t>(value >> 24U);
}

// The state hash after each frame when every peer's input is known in time.
std::vector<std::uint64_t> lockstep(int slots) {
  Game game{slots};
  std::vector<std::uint64_t> hashes;
  hashes.reserve(static_cast<std::size_t>(kFrames));
  for (std::int64_t frame = 0; frame < kFrames; ++frame) {
    std::vector<std::uint8_t> all;
    all.reserve(static_cast<std::size_t>(slots));
    for (int slot = 0; slot < slots; ++slot) {
      all.push_back(input_of(slot, frame));
    }
    game.setInputs(all);
    game.advance(false);
    hashes.push_back(game.hash());
  }
  return hashes;
}

// One direction of a link: each message arrives `delay` ticks later, give or take one, unless
// dropped.
struct LinkQuality {
  int delay = 0;
  double loss = 0;
};

class Wire {
public:
  Wire(LinkQuality quality, std::uint32_t seed)
      : delay_{quality.delay}, loss_{quality.loss}, random_{seed} {
  }
  void send(std::int64_t tick, std::string message) {
    if (std::uniform_real_distribution<double>{0, 1}(random_) < loss_) {
      return;
    }
    const std::int64_t jitter = std::uniform_int_distribution<int>{-1, 1}(random_);
    queue_.push_back({tick + delay_ + jitter, std::move(message)});
  }
  std::vector<std::string> arrived(std::int64_t tick) {
    std::vector<std::string> out;
    for (auto each = queue_.begin(); each != queue_.end();) {
      if (each->first <= tick) {
        out.push_back(std::move(each->second));
        each = queue_.erase(each);
      } else {
        ++each;
      }
    }
    return out;
  }

private:
  int delay_;
  double loss_;
  std::mt19937 random_;
  std::deque<std::pair<std::int64_t, std::string>> queue_;
};

struct Peer {
  std::unique_ptr<Game> game;
  std::unique_ptr<Session> session;
  int slot = 0;
  // Per link: the other peer's index and the wire toward it.
  std::vector<int> links;
};

struct Network {
  std::vector<Peer> peers;
  // wires[from][to]
  std::vector<std::vector<std::unique_ptr<Wire>>> wires;
};

// Peers 0..count-1, each driving its own slot; `star` links everyone to peer 0 only.
Network network(int count, bool star, LinkQuality quality) {
  Network made;
  made.wires.resize(static_cast<std::size_t>(count));
  for (int from = 0; from < count; ++from) {
    made.wires[static_cast<std::size_t>(from)].resize(static_cast<std::size_t>(count));
    for (int to = 0; to < count; ++to) {
      made.wires[static_cast<std::size_t>(from)][static_cast<std::size_t>(to)] =
          std::make_unique<Wire>(quality, static_cast<std::uint32_t>(from * 31 + to + 1));
    }
  }
  for (int index = 0; index < count; ++index) {
    Peer peer;
    peer.slot = index;
    for (int other = 0; other < count; ++other) {
      const bool linked = other != index && (!star || index == 0 || other == 0);
      if (linked) {
        peer.links.push_back(other);
      }
    }
    peer.game = std::make_unique<Game>(count);
    Config config;
    config.slots = count;
    config.input_bytes = 1;
    config.local_slots = {index};
    config.input_delay = kDelay;
    config.max_rollback = kRollback;
    peer.session =
        std::make_unique<Session>(config, *peer.game, static_cast<int>(peer.links.size()));
    made.peers.push_back(std::move(peer));
  }
  return made;
}

// Ticks until every peer has run `frames` frames and confirmed them all.
void play(Network &net, std::int64_t frames) {
  for (std::int64_t tick = 0; tick < frames * 4; ++tick) {
    bool done = true;
    for (std::size_t index = 0; index < net.peers.size(); ++index) {
      Peer &peer = net.peers[index];
      for (std::size_t link = 0; link < peer.links.size(); ++link) {
        Wire &wire = *net.wires[static_cast<std::size_t>(peer.links[link])][index];
        for (const std::string &message : wire.arrived(tick)) {
          peer.session->receive(static_cast<int>(link), message);
        }
      }
      if (peer.session->frame() < frames + kRollback + kDelay + 4) {
        const std::array<std::uint8_t, 1> local{
            input_of(peer.slot, peer.session->frame() + kDelay)};
        peer.session->tick(local);
      }
      for (std::size_t link = 0; link < peer.links.size(); ++link) {
        net.wires[index][static_cast<std::size_t>(peer.links[link])]->send(
            tick, peer.session->message_for(static_cast<int>(link)));
      }
      done = done && peer.session->confirmed_frame() >= frames;
    }
    if (done) {
      return;
    }
  }
  assert(false && "the peers never confirmed every frame");
}

void peers_match_lockstep(int count, bool star) {
  constexpr std::int64_t frames = kFrames;
  Network net = network(count, star, {.delay = 3, .loss = 0.2});
  play(net, frames);
  const std::vector<std::uint64_t> truth = lockstep(count);
  std::int64_t resimulated = 0;
  for (const Peer &peer : net.peers) {
    // The session keeps its newest final hashes.
    for (std::int64_t frame = frames - 100; frame < frames; ++frame) {
      assert(peer.session->final_hash(frame) == truth[static_cast<std::size_t>(frame)]);
    }
    assert(!peer.session->desync_frame());
    resimulated += peer.session->resimulated_frames();
  }
  assert(resimulated > 0);
  std::printf("%d peers (%s): lockstep hashes matched, %lld frames resimulated\n", count,
              star ? "star" : "mesh", static_cast<long long>(resimulated));
}

void a_changed_state_is_a_desync() {
  Network net = network(2, false, {.delay = 2, .loss = 0.0});
  play(net, 200);
  const std::int64_t at = net.peers[1].session->frame() + 20;
  net.peers[1].game->drift_from(at);
  bool reported = false;
  for (std::int64_t tick = 0; tick < 200 && !reported; ++tick) {
    for (std::size_t index = 0; index < 2; ++index) {
      Peer &peer = net.peers[index];
      for (const std::string &message : net.wires[1 - index][index]->arrived(1000 + tick)) {
        peer.session->receive(0, message);
      }
      const std::array<std::uint8_t, 1> local{input_of(peer.slot, peer.session->frame() + kDelay)};
      reported = reported || peer.session->tick(local) == Status::Desynced;
      net.wires[index][1 - index]->send(1000 + tick, peer.session->message_for(0));
    }
  }
  assert(reported);
  const auto seen = net.peers[0].session->desync_frame().value_or(
      net.peers[1].session->desync_frame().value_or(-1));
  assert(seen >= at && seen <= at + 40);
  std::printf("a drifting game was reported as a desync at frame %lld (it drifts from %lld)\n",
              static_cast<long long>(seen), static_cast<long long>(at));
}

void a_peer_that_hears_nothing_stalls() {
  Game game{2};
  Config config;
  config.slots = 2;
  config.local_slots = {0};
  config.input_delay = kDelay;
  config.max_rollback = kRollback;
  Session session{config, game, 1};
  const std::array<std::uint8_t, 1> local{1};
  int advanced = 0;
  for (int tick = 0; tick < 50; ++tick) {
    advanced += session.tick(local) == Status::Advanced ? 1 : 0;
  }
  assert(advanced == kRollback + 1 && game.presented() == kRollback + 1);
}

} // namespace

int main() {
  peers_match_lockstep(2, false);
  peers_match_lockstep(4, true);
  a_changed_state_is_a_desync();
  a_peer_that_hears_nothing_stalls();
  std::puts("rollback sessions match lockstep, rewind, report desyncs and stall");
  return 0;
}

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lucent/p2p/packet_seal.h"
#include "lucent/p2p/path.h"

// Ordered messages between two peers over sealed datagrams on a path, for what must arrive before
// a game starts. Acks, a window and resends, with keepalives that measure the round trip and notice
// a peer gone. The application names its message kinds and caps each: a size past its kind's cap,
// or a kind it did not name, breaks the link before anything is allocated.
namespace lucent::p2p {

// The most each message kind (its index) may hold; 0 for a kind that is not sent.
using LinkCaps = std::array<std::size_t, 256>;

// This side's key and the peer's.
struct LinkKeys {
  std::span<const std::uint8_t> send;
  std::span<const std::uint8_t> receive;
};

struct LinkMessage {
  std::uint8_t kind = 0;
  std::string body;
};

class ReliableLink {
public:
  using Clock = std::chrono::steady_clock;

  // The largest payload of one datagram, under a usual MTU with the seal around it.
  static constexpr std::size_t kChunkBytes = 1100;
  // How many datagrams may be unacknowledged, and held out of order.
  static constexpr std::size_t kWindow = 32;
  // The sealed kind its datagrams carry.
  static constexpr std::uint8_t kSealKind = 13;

  // Talks to `peer` on `path`, which must outlive this.
  ReliableLink(Path &path, PathAddress peer, const LinkKeys &keys, const LinkCaps &caps);

  // Queues a message; `body` must fit `kind`'s cap.
  void send(std::uint8_t kind, std::string_view body);
  // Sends what is due and takes what came, waiting at most `wait`.
  void pump(std::chrono::milliseconds wait);
  std::optional<LinkMessage> take();
  bool flushed() const noexcept {
    return outgoing_.empty() && unacked_.empty();
  }
  // Nothing has been heard for too long.
  bool silent() const noexcept;
  // The peer sent what this application does not send.
  bool broken() const noexcept {
    return broken_;
  }
  std::optional<std::chrono::milliseconds> round_trip() const noexcept {
    return round_trip_;
  }
  // Where the peer was last heard from, which a NAT may have moved.
  const PathAddress &peer() const noexcept {
    return peer_;
  }

private:
  struct Unacked {
    std::uint32_t sequence = 0;
    std::string chunk;
    Clock::time_point sent;
  };

  void send_sealed(std::string_view payload);
  void send_chunks();
  void send_data(std::uint32_t sequence, std::string_view chunk);
  void send_ack();
  void send_ping(Clock::time_point now);
  void take_datagram(std::string_view payload);
  void take_data(std::string_view payload);
  void take_ack(std::string_view payload);
  void take_pong(std::string_view payload);
  void parse();

  Path &path_;
  PathAddress peer_;
  PacketSeal seal_;
  PacketOpener opener_;
  LinkCaps caps_;
  std::vector<char> buffer_;
  std::string outgoing_;
  std::deque<Unacked> unacked_;
  std::uint32_t next_sequence_ = 0;
  std::map<std::uint32_t, std::string> early_;
  std::uint32_t expected_ = 0;
  std::string incoming_;
  std::deque<LinkMessage> arrived_;
  Clock::time_point started_{Clock::now()};
  Clock::time_point heard_{started_};
  Clock::time_point pinged_{};
  std::optional<std::chrono::milliseconds> round_trip_;
  bool broken_ = false;
};

} // namespace lucent::p2p

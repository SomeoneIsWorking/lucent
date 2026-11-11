#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lucent/p2p/crypto.h"
#include "lucent/p2p/packet_seal.h"
#include "lucent/p2p/path.h"

// Two machines finding each other from a code on one path, whichever backend it is. What they send
// before they share session keys is sealed under a key from the code's secret with a random nonce:
// the friend's Hello and the host's Welcome carry ephemeral X25519 keys, and the host's Knock opens
// its NAT toward the addresses in a reply code. A friend waiting on a reply code sends only far
// enough to hold its own NAT open: a datagram that reaches a Linux NAT first leaves an entry that
// moves the host's public port when it answers. The host takes the friend as its peer only when a
// datagram opens under the session keys, and the code is spent then. Whatever does not open is
// dropped.
namespace lucent::p2p {

enum class Role : std::uint8_t { Host, Friend };

// The keys a meeting agreed, from which each purpose derives one key per sender, so no counter
// repeats under a key.
class SessionKeys {
public:
  SessionKeys(crypto::Bytes material, crypto::Bytes salt, std::string label);
  ~SessionKeys();
  SessionKeys(const SessionKeys &) = default;
  SessionKeys &operator=(const SessionKeys &) = default;
  SessionKeys(SessionKeys &&) noexcept = default;
  SessionKeys &operator=(SessionKeys &&) noexcept = default;

  // The key `sender` seals `purpose` with: "<label>\n<purpose>\nhost" or "...\nfriend".
  crypto::Bytes key(std::string_view purpose, Role sender) const;
  bool operator==(const SessionKeys &) const = default;

private:
  crypto::Bytes material_;
  crypto::Bytes salt_;
  std::string label_;
};

struct Found {
  PathAddress peer;
  SessionKeys keys;
};

// The first byte of a datagram on a path, before any session key: the handshake's own kinds; STUN
// starts with 0 or 1, and a sealed datagram with `kSealVersion`.
inline constexpr std::uint8_t kHelloKind = 0xE1;
inline constexpr std::uint8_t kWelcomeKind = 0xE2;
inline constexpr std::uint8_t kKnockKind = 0xE3;

class Handshake {
public:
  using Clock = std::chrono::steady_clock;

  // Sends on `path`, which must outlive this. `secret` is the invite's; `label` separates one
  // application's keys from another's ("opensu-online").
  Handshake(Path &path, Role role, std::span<const std::uint8_t> secret, std::string label);
  ~Handshake();
  Handshake(const Handshake &) = delete;
  Handshake &operator=(const Handshake &) = delete;
  Handshake(Handshake &&) = delete;
  Handshake &operator=(Handshake &&) = delete;

  // Adds where the other machine may be, to send to from now on.
  void aim(const std::vector<PathAddress> &addresses);
  // Sends only far enough to hold this side's NAT open, until the other machine is heard.
  void keep_near();
  // Sends what is due and takes what came, waiting at most `wait`.
  void step(std::chrono::milliseconds wait);
  // The other machine, once found; `step` does nothing more after that.
  const std::optional<Found> &found() const noexcept {
    return found_;
  }
  // Datagrams dropped for not opening, or for arriving once the code was spent.
  std::uint64_t dropped() const noexcept {
    return dropped_;
  }

private:
  struct Caller {
    SessionKeys keys;
    std::unique_ptr<PacketOpener> opener;
  };

  std::string sealed(std::uint8_t kind, std::string_view plain) const;
  std::optional<std::string> opened(std::uint8_t kind, std::string_view datagram) const;
  void take(std::string_view datagram, const PathAddress &from);
  void take_hello(std::string_view datagram, const PathAddress &from);
  void take_welcome(std::string_view datagram, const PathAddress &from);
  void take_knock(std::string_view datagram, const PathAddress &from);
  void take_sealed(std::string_view datagram, const PathAddress &from);
  void send_due();
  SessionKeys keys_with(std::span<const std::uint8_t> shared,
                        std::span<const std::uint8_t> host_key,
                        std::span<const std::uint8_t> friend_key) const;

  Path &path_;
  Role role_;
  std::string label_;
  crypto::Bytes secret_;
  crypto::Bytes hello_key_;
  crypto::KeyPair own_;
  std::vector<PathAddress> targets_;
  std::map<crypto::Bytes, Caller> callers_;
  std::optional<Found> found_;
  Clock::time_point next_send_{};
  bool near_ = false;
  std::uint64_t dropped_ = 0;
  std::vector<char> buffer_;
};

// The purpose a handshake's own first sealed datagram uses: the friend's link key.
inline constexpr std::string_view kLinkPurpose = "link";

} // namespace lucent::p2p

#include "lucent/p2p/handshake.h"

#include <algorithm>
#include <utility>

namespace lucent::p2p {
namespace {

using namespace std::chrono_literals;
using crypto::Bytes;

constexpr std::size_t kNonceAt = 1;
constexpr std::size_t kSealedAt = kNonceAt + crypto::kSealNonceBytes;
// How often each side sends to the other's addresses while they look for each other.
constexpr auto kSendEvery = 100ms;
// Past a home router and a carrier's NAT, and short of most far ends.
constexpr int kNearHops = 3;
// How many addresses and unproved friends are kept, whatever arrives.
constexpr std::size_t kMostTargets = 12;
constexpr std::size_t kMostCallers = 4;

Bytes joined(std::span<const std::uint8_t> first, std::span<const std::uint8_t> second) {
  Bytes out{first.begin(), first.end()};
  out.insert(out.end(), second.begin(), second.end());
  return out;
}

} // namespace

SessionKeys::SessionKeys(Bytes material, Bytes salt, std::string label)
    : material_{std::move(material)}, salt_{std::move(salt)}, label_{std::move(label)} {
}

SessionKeys::~SessionKeys() {
  crypto::wipe(material_);
}

Bytes SessionKeys::key(std::string_view purpose, Role sender) const {
  const std::string info =
      label_ + '\n' + std::string{purpose} + '\n' + (sender == Role::Host ? "host" : "friend");
  return crypto::hkdf_sha256(material_, salt_, info, crypto::kKeyBytes);
}

Handshake::Handshake(Path &path, Role role, std::span<const std::uint8_t> secret, std::string label)
    : path_{path}, role_{role}, label_{std::move(label)}, secret_{secret.begin(), secret.end()},
      hello_key_{crypto::hkdf_sha256(secret, crypto::bytes_of(label_ + "\nhello"),
                                     label_ + "\nhello", crypto::kKeyBytes)},
      buffer_(net::kLargestDatagram) {
}

Handshake::~Handshake() {
  crypto::wipe(secret_);
  crypto::wipe(hello_key_);
}

SessionKeys Handshake::keys_with(std::span<const std::uint8_t> shared,
                                 std::span<const std::uint8_t> host_key,
                                 std::span<const std::uint8_t> friend_key) const {
  return SessionKeys{joined(shared, secret_), joined(host_key, friend_key), label_};
}

std::string Handshake::sealed(std::uint8_t kind, std::string_view plain) const {
  const Bytes nonce = crypto::random_bytes(crypto::kSealNonceBytes);
  const std::string header(1, static_cast<char>(kind));
  return header + std::string{crypto::text_of(nonce)} +
         crypto::seal(hello_key_, nonce, header, plain);
}

std::optional<std::string> Handshake::opened(std::uint8_t kind, std::string_view datagram) const {
  if (datagram.size() < kSealedAt + crypto::kSealTagBytes) {
    return std::nullopt;
  }
  return crypto::open(hello_key_, crypto::bytes_of(datagram.substr(kNonceAt, kSealedAt - kNonceAt)),
                      std::string(1, static_cast<char>(kind)), datagram.substr(kSealedAt));
}

void Handshake::aim(const std::vector<PathAddress> &addresses) {
  for (const PathAddress &address : addresses) {
    if (targets_.size() < kMostTargets &&
        std::find(targets_.begin(), targets_.end(), address) == targets_.end()) {
      targets_.push_back(address);
    }
  }
  next_send_ = {};
}

void Handshake::keep_near() {
  near_ = true;
}

void Handshake::send_due() {
  const Clock::time_point now = Clock::now();
  if (now < next_send_) {
    return;
  }
  next_send_ = now + kSendEvery;
  const std::string message = role_ == Role::Friend
                                  ? sealed(kHelloKind, crypto::text_of(own_.public_key()))
                                  : sealed(kKnockKind, {});
  for (const PathAddress &target : targets_) {
    if (near_) {
      path_.send_within(target, message, kNearHops);
    } else {
      path_.send(target, message);
    }
  }
}

void Handshake::step(std::chrono::milliseconds wait) {
  if (found_) {
    return;
  }
  send_due();
  PathAddress from;
  std::optional<std::size_t> length = path_.receive(buffer_, from, wait);
  while (length && !found_) {
    take({buffer_.data(), *length}, from);
    length = path_.receive(buffer_, from, 0ms);
  }
}

std::string Handshake::take_other() {
  return std::exchange(other_, {});
}

void Handshake::take(std::string_view datagram, const PathAddress &from) {
  if (datagram.empty()) {
    ++dropped_;
    return;
  }
  const auto kind = static_cast<std::uint8_t>(datagram.front());
  if (kind == kHelloKind && role_ == Role::Host) {
    take_hello(datagram, from);
  } else if (kind == kWelcomeKind && role_ == Role::Friend) {
    take_welcome(datagram, from);
  } else if (kind == kKnockKind && role_ == Role::Friend) {
    take_knock(datagram, from);
  } else if (kind == kSealVersion && role_ == Role::Host) {
    take_sealed(datagram, from);
  } else if (kind != kHelloKind && kind != kWelcomeKind && kind != kKnockKind &&
             kind != kSealVersion) {
    other_ = datagram;
  } else {
    ++dropped_;
  }
}

void Handshake::take_hello(std::string_view datagram, const PathAddress &from) {
  const std::optional<std::string> plain = opened(kHelloKind, datagram);
  if (!plain || plain->size() != crypto::kKeyBytes) {
    ++dropped_;
    return;
  }
  const std::span<const std::uint8_t> friend_key = crypto::bytes_of(*plain);
  const Bytes key{friend_key.begin(), friend_key.end()};
  if (!callers_.contains(key)) {
    const std::optional<Bytes> shared = own_.agree(friend_key);
    if (!shared || callers_.size() >= kMostCallers) {
      ++dropped_;
      return;
    }
    SessionKeys keys = keys_with(*shared, own_.public_key(), friend_key);
    auto opener = std::make_unique<PacketOpener>(keys.key(kLinkPurpose, Role::Friend));
    callers_.emplace(key, Caller{std::move(keys), std::move(opener)});
  }
  const std::string answer = std::string{crypto::text_of(own_.public_key())} + *plain;
  path_.send(from, sealed(kWelcomeKind, answer));
}

void Handshake::take_welcome(std::string_view datagram, const PathAddress &from) {
  const std::optional<std::string> plain = opened(kWelcomeKind, datagram);
  if (!plain || plain->size() != 2 * crypto::kKeyBytes) {
    ++dropped_;
    return;
  }
  const std::span<const std::uint8_t> keys = crypto::bytes_of(*plain);
  const std::span<const std::uint8_t> host_key = keys.first(crypto::kKeyBytes);
  if (!crypto::same_bytes(keys.last(crypto::kKeyBytes), own_.public_key())) {
    ++dropped_;
    return;
  }
  const std::optional<Bytes> shared = own_.agree(host_key);
  if (!shared) {
    ++dropped_;
    return;
  }
  found_ = Found{from, keys_with(*shared, host_key, own_.public_key())};
}

void Handshake::take_knock(std::string_view datagram, const PathAddress &from) {
  if (!opened(kKnockKind, datagram)) {
    ++dropped_;
    return;
  }
  aim({from});
  near_ = false;
  next_send_ = {};
}

void Handshake::take_sealed(std::string_view datagram, const PathAddress &from) {
  for (auto &[key, caller] : callers_) {
    if (caller.opener->open(datagram)) {
      found_ = Found{from, std::move(caller.keys)};
      callers_.clear();
      return;
    }
  }
  ++dropped_;
}

} // namespace lucent::p2p

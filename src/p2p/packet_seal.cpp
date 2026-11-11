#include "lucent/p2p/packet_seal.h"

#include <array>

#include "lucent/p2p/crypto.h"
#include "wire_bytes.h"

namespace lucent::p2p {
namespace {

constexpr std::size_t kCounterAt = 2;
constexpr std::size_t kCounterBytes = 8;

// Four zero bytes, then the counter.
std::array<std::uint8_t, crypto::kSealNonceBytes> nonce_of(std::uint64_t counter) {
  std::array<std::uint8_t, crypto::kSealNonceBytes> nonce{};
  for (std::size_t i = 0; i < kCounterBytes; ++i) {
    nonce[crypto::kSealNonceBytes - kCounterBytes + i] =
        static_cast<std::uint8_t>((counter >> (8 * i)) & 0xFFu);
  }
  return nonce;
}

} // namespace

PacketSeal::PacketSeal(std::span<const std::uint8_t> key) : key_{key.begin(), key.end()} {
}

PacketSeal::~PacketSeal() {
  crypto::wipe(key_);
}

std::string PacketSeal::seal(std::uint8_t kind, std::string_view payload) {
  const std::uint64_t counter = counter_++;
  std::string header;
  header.reserve(kSealHeaderBytes);
  header.push_back(static_cast<char>(kSealVersion));
  header.push_back(static_cast<char>(kind));
  wire::append_le<kCounterBytes>(header, counter);
  return header + crypto::seal(key_, nonce_of(counter), header, payload);
}

PacketOpener::PacketOpener(std::span<const std::uint8_t> key) : key_{key.begin(), key.end()} {
}

PacketOpener::~PacketOpener() {
  crypto::wipe(key_);
}

std::optional<Opened> PacketOpener::open(std::string_view datagram) {
  if (datagram.size() < kSealOverhead || static_cast<std::uint8_t>(datagram[0]) != kSealVersion) {
    return std::nullopt;
  }
  const std::uint64_t counter = wire::le_at<kCounterBytes>(datagram, kCounterAt);
  if (!fresh(counter)) {
    return std::nullopt;
  }
  std::optional<std::string> payload =
      crypto::open(key_, nonce_of(counter), datagram.substr(0, kSealHeaderBytes),
                   datagram.substr(kSealHeaderBytes));
  if (!payload) {
    return std::nullopt;
  }
  take(counter);
  return Opened{static_cast<std::uint8_t>(datagram[1]), counter, std::move(*payload)};
}

bool PacketOpener::fresh(std::uint64_t counter) const {
  if (!newest_ || counter > *newest_) {
    return true;
  }
  return *newest_ - counter < kWindow && !seen_[counter % kWindow];
}

void PacketOpener::take(std::uint64_t counter) {
  if (!newest_ || counter > *newest_) {
    if (!newest_ || counter - *newest_ >= kWindow) {
      seen_.reset();
    } else {
      for (std::uint64_t skipped = *newest_ + 1; skipped < counter; ++skipped) {
        seen_.reset(skipped % kWindow);
      }
    }
    newest_ = counter;
  }
  seen_.set(counter % kWindow);
}

} // namespace lucent::p2p

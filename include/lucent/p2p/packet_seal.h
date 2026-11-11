#pragma once

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// One datagram sealed on its own: a clear header (version, the application's kind byte, the
// sender's counter), then the payload under ChaCha20-Poly1305 with the sender's key and the counter
// as nonce, the header authenticated with it. The opener takes each counter once.
namespace lucent::p2p {

inline constexpr std::uint8_t kSealVersion = 3;
inline constexpr std::size_t kSealHeaderBytes = 10;
// What a sealed datagram adds to its payload.
inline constexpr std::size_t kSealOverhead = kSealHeaderBytes + 16;

class PacketSeal {
public:
  explicit PacketSeal(std::span<const std::uint8_t> key);
  ~PacketSeal();
  PacketSeal(const PacketSeal &) = delete;
  PacketSeal &operator=(const PacketSeal &) = delete;
  PacketSeal(PacketSeal &&) = delete;
  PacketSeal &operator=(PacketSeal &&) = delete;

  // `payload` as the next datagram of `kind`. Not thread safe.
  std::string seal(std::uint8_t kind, std::string_view payload);

private:
  std::vector<std::uint8_t> key_;
  std::uint64_t counter_ = 0;
};

struct Opened {
  std::uint8_t kind = 0;
  std::uint64_t counter = 0;
  std::string payload;
};

class PacketOpener {
public:
  // How many counters behind the newest are still taken, reordered.
  static constexpr std::size_t kWindow = 4096;

  explicit PacketOpener(std::span<const std::uint8_t> key);
  ~PacketOpener();
  PacketOpener(const PacketOpener &) = delete;
  PacketOpener &operator=(const PacketOpener &) = delete;
  PacketOpener(PacketOpener &&) = delete;
  PacketOpener &operator=(PacketOpener &&) = delete;

  // Nothing when `datagram` was not sealed with the key, was changed, or its counter was taken
  // already or is older than the window. Not thread safe.
  std::optional<Opened> open(std::string_view datagram);

private:
  bool fresh(std::uint64_t counter) const;
  void take(std::uint64_t counter);

  std::vector<std::uint8_t> key_;
  std::optional<std::uint64_t> newest_;
  std::bitset<kWindow> seen_;
};

} // namespace lucent::p2p

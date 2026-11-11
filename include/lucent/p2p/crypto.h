#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The primitives lucent::p2p is built from, on OpenSSL libcrypto.
namespace lucent::p2p::crypto {

using Bytes = std::vector<std::uint8_t>;

// An X25519 key and every derived key is this long.
inline constexpr std::size_t kKeyBytes = 32;
inline constexpr std::size_t kSealNonceBytes = 12;
inline constexpr std::size_t kSealTagBytes = 16;

std::span<const std::uint8_t> bytes_of(std::string_view text) noexcept;
std::string_view text_of(std::span<const std::uint8_t> bytes) noexcept;

// `count` bytes from the system's generator; throws std::runtime_error when it has none.
Bytes random_bytes(std::size_t count);

Bytes sha256(std::span<const std::uint8_t> data);
Bytes hmac_sha256(std::span<const std::uint8_t> key, std::span<const std::uint8_t> message);
Bytes hkdf_sha256(std::span<const std::uint8_t> material, std::span<const std::uint8_t> salt,
                  std::string_view label, std::size_t length);

// ChaCha20-Poly1305: the ciphertext with its tag after it.
std::string seal(std::span<const std::uint8_t> key, std::span<const std::uint8_t> nonce,
                 std::string_view header, std::string_view plain);
// Nothing when `sealed` was not sealed under `key`, `nonce` and `header`, or was changed.
std::optional<std::string> open(std::span<const std::uint8_t> key,
                                std::span<const std::uint8_t> nonce, std::string_view header,
                                std::string_view sealed);

bool same_bytes(std::span<const std::uint8_t> first, std::span<const std::uint8_t> second) noexcept;
// Overwrites `bytes` in a way the compiler keeps.
void wipe(std::span<std::uint8_t> bytes) noexcept;

// An ephemeral X25519 key pair.
class KeyPair {
public:
  KeyPair();
  ~KeyPair();
  KeyPair(const KeyPair &) = delete;
  KeyPair &operator=(const KeyPair &) = delete;
  KeyPair(KeyPair &&) = delete;
  KeyPair &operator=(KeyPair &&) = delete;

  std::span<const std::uint8_t> public_key() const noexcept {
    return public_key_;
  }
  // The shared secret with `peer`; nothing for a key that is not one, or yields all zeros.
  std::optional<Bytes> agree(std::span<const std::uint8_t> peer) const;

private:
  struct Key;
  std::unique_ptr<Key> key_;
  std::array<std::uint8_t, kKeyBytes> public_key_{};
};

} // namespace lucent::p2p::crypto

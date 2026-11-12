#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "lucent/p2p/crypto.h"

// A short message to the holder of one X25519 key, left in a directory's mailbox for that key. A
// letter is hex of: version, the sender's public key, a random nonce, and ChaCha20-Poly1305 of
// the text under HKDF-SHA256 of the two keys' X25519 secret, salted with the nonce, with the
// purpose and both public keys in the HKDF info and both keys and the version as associated data.
// Only the two key holders can open it, and a letter sealed for one purpose does not open as
// another.
namespace lucent::p2p {

inline constexpr std::uint8_t kLetterVersion = 1;
inline constexpr std::size_t kMaxLetterPlainBytes = 3072;
// What a letter is for, sealed into its key: an ask does not open as an answer.
enum class LetterPurpose : std::uint8_t { Ask, Answer };

// The longest letter text a directory carries.
inline constexpr std::size_t kMaxLetterTextBytes =
    2 * (1 + crypto::kKeyBytes + crypto::kSealNonceBytes + kMaxLetterPlainBytes +
         crypto::kSealTagBytes);
// A letter lapses this long after it is posted, in every directory.
inline constexpr std::int64_t kLetterSeconds = 120;

// A mailbox is named by its key: 64 lowercase hex digits.
std::string mailbox_of(std::span<const std::uint8_t> public_key);
bool valid_mailbox(std::string_view mailbox) noexcept;

// Throws std::invalid_argument for a `plain` over kMaxLetterPlainBytes or a `to` that is not an
// X25519 key with which `from` agrees.
std::string write_letter(const crypto::KeyPair &from, std::span<const std::uint8_t> to,
                         LetterPurpose purpose, std::string_view plain);

struct OpenedLetter {
  crypto::Bytes from;
  std::string plain;
};
// Nothing for text past kMaxLetterTextBytes (checked before anything else), not hex, another
// version, or not sealed to `to` for `purpose`.
std::optional<OpenedLetter> read_letter(const crypto::KeyPair &to, std::string_view text,
                                        LetterPurpose purpose);

} // namespace lucent::p2p

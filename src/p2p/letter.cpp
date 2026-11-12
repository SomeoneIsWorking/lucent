#include "lucent/p2p/letter.h"

#include "lucent/text.h"

#include <stdexcept>

namespace lucent::p2p {
namespace {

constexpr std::size_t kHeaderBytes = 1 + crypto::kKeyBytes + crypto::kSealNonceBytes;

std::string bytes_text(std::span<const std::uint8_t> bytes) {
  return std::string{crypto::text_of(bytes)};
}

// The associated data: version, sender, recipient.
std::string header(std::span<const std::uint8_t> from, std::span<const std::uint8_t> to) {
  return std::string(1, static_cast<char>(kLetterVersion)) + bytes_text(from) + bytes_text(to);
}

enum class Side : std::uint8_t { Sender, Recipient };

std::string_view name_of(LetterPurpose purpose) {
  return purpose == LetterPurpose::Ask ? "ask" : "answer";
}

// `own` is the letter's `side`; `peer` the other end. The nonce salts it, so each letter has a
// key of its own.
std::optional<crypto::Bytes> letter_key(const crypto::KeyPair &own,
                                        std::span<const std::uint8_t> peer, Side side,
                                        std::span<const std::uint8_t> nonce,
                                        LetterPurpose purpose) {
  std::optional<crypto::Bytes> shared = own.agree(peer);
  if (!shared) {
    return std::nullopt;
  }
  const bool sending = side == Side::Sender;
  const std::string info = "lucent letter v1\n" + std::string{name_of(purpose)} + "\n" +
                           bytes_text(sending ? own.public_key() : peer) +
                           bytes_text(sending ? peer : own.public_key());
  crypto::Bytes key = crypto::hkdf_sha256(*shared, nonce, info, crypto::kKeyBytes);
  crypto::wipe(*shared);
  return key;
}

} // namespace

std::string mailbox_of(std::span<const std::uint8_t> public_key) {
  return text::hex(public_key);
}

bool valid_mailbox(std::string_view mailbox) noexcept {
  if (mailbox.size() != 2 * crypto::kKeyBytes) {
    return false;
  }
  for (const char c : mailbox) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

std::string write_letter(const crypto::KeyPair &from, std::span<const std::uint8_t> to,
                         LetterPurpose purpose, std::string_view plain) {
  if (plain.size() > kMaxLetterPlainBytes) {
    throw std::invalid_argument{"a letter holds at most 3072 bytes"};
  }
  const crypto::Bytes nonce = crypto::random_bytes(crypto::kSealNonceBytes);
  std::optional<crypto::Bytes> key = letter_key(from, to, Side::Sender, nonce, purpose);
  if (!key) {
    throw std::invalid_argument{"not a key to write to"};
  }
  const std::string sealed = crypto::seal(*key, nonce, header(from.public_key(), to), plain);
  crypto::wipe(*key);
  const std::string out = std::string(1, static_cast<char>(kLetterVersion)) +
                          bytes_text(from.public_key()) + bytes_text(nonce) + sealed;
  return text::hex(crypto::bytes_of(out));
}

std::optional<OpenedLetter> read_letter(const crypto::KeyPair &to, std::string_view text,
                                        LetterPurpose purpose) {
  if (text.size() > kMaxLetterTextBytes ||
      text.size() < 2 * (kHeaderBytes + crypto::kSealTagBytes)) {
    return std::nullopt;
  }
  const std::optional<std::vector<std::uint8_t>> bytes = text::from_hex(text);
  if (!bytes || (*bytes)[0] != kLetterVersion) {
    return std::nullopt;
  }
  const std::span<const std::uint8_t> all{*bytes};
  const std::span<const std::uint8_t> from = all.subspan(1, crypto::kKeyBytes);
  const std::span<const std::uint8_t> nonce =
      all.subspan(1 + crypto::kKeyBytes, crypto::kSealNonceBytes);
  std::optional<crypto::Bytes> key = letter_key(to, from, Side::Recipient, nonce, purpose);
  if (!key) {
    return std::nullopt;
  }
  std::optional<std::string> plain = crypto::open(*key, nonce, header(from, to.public_key()),
                                                  crypto::text_of(all.subspan(kHeaderBytes)));
  crypto::wipe(*key);
  if (!plain) {
    return std::nullopt;
  }
  return OpenedLetter{.from = crypto::Bytes(from.begin(), from.end()), .plain = std::move(*plain)};
}

} // namespace lucent::p2p

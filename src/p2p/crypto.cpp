#include "lucent/p2p/crypto.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace lucent::p2p::crypto {
namespace {

struct FreeCipher {
  void operator()(EVP_CIPHER_CTX *context) const noexcept {
    EVP_CIPHER_CTX_free(context);
  }
};
struct FreeKey {
  void operator()(EVP_PKEY *key) const noexcept {
    EVP_PKEY_free(key);
  }
};
struct FreeKeyContext {
  void operator()(EVP_PKEY_CTX *context) const noexcept {
    EVP_PKEY_CTX_free(context);
  }
};

using Cipher = std::unique_ptr<EVP_CIPHER_CTX, FreeCipher>;
using KeyContext = std::unique_ptr<EVP_PKEY_CTX, FreeKeyContext>;

const unsigned char *data_of(std::string_view text) noexcept {
  return reinterpret_cast<const unsigned char *>(text.data());
}

int length_of(std::size_t size) {
  if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::length_error{"lucent::p2p::crypto input too large"};
  }
  return static_cast<int>(size);
}

Cipher cipher_for(std::span<const std::uint8_t> key, std::span<const std::uint8_t> nonce,
                  bool encrypt) {
  if (key.size() != kKeyBytes || nonce.size() != kSealNonceBytes) {
    return nullptr;
  }
  Cipher context{EVP_CIPHER_CTX_new()};
  if (!context || EVP_CipherInit_ex(context.get(), EVP_chacha20_poly1305(), nullptr, key.data(),
                                    nonce.data(), encrypt ? 1 : 0) != 1) {
    return nullptr;
  }
  return context;
}

} // namespace

std::span<const std::uint8_t> bytes_of(std::string_view text) noexcept {
  return {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()};
}

std::string_view text_of(std::span<const std::uint8_t> bytes) noexcept {
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

Bytes random_bytes(std::size_t count) {
  Bytes bytes(count);
  if (count > 0 && RAND_bytes(bytes.data(), length_of(count)) != 1) {
    throw std::runtime_error{"the system has no random bytes to give"};
  }
  return bytes;
}

Bytes sha256(std::span<const std::uint8_t> data) {
  Bytes digest(SHA256_DIGEST_LENGTH);
  SHA256(data.data(), data.size(), digest.data());
  return digest;
}

Bytes hmac_sha256(std::span<const std::uint8_t> key, std::span<const std::uint8_t> message) {
  Bytes digest(SHA256_DIGEST_LENGTH);
  unsigned length = 0;
  if (HMAC(EVP_sha256(), key.data(), length_of(key.size()), message.data(), message.size(),
           digest.data(), &length) == nullptr) {
    throw std::runtime_error{"HMAC-SHA-256 failed"};
  }
  return digest;
}

Bytes hkdf_sha256(std::span<const std::uint8_t> material, std::span<const std::uint8_t> salt,
                  std::string_view label, std::size_t length) {
  const KeyContext context{EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr)};
  Bytes out(length);
  std::size_t size = length;
  if (!context || EVP_PKEY_derive_init(context.get()) != 1 ||
      EVP_PKEY_CTX_set_hkdf_md(context.get(), EVP_sha256()) != 1 ||
      EVP_PKEY_CTX_set1_hkdf_salt(context.get(), salt.data(), length_of(salt.size())) != 1 ||
      EVP_PKEY_CTX_set1_hkdf_key(context.get(), material.data(), length_of(material.size())) != 1 ||
      EVP_PKEY_CTX_add1_hkdf_info(context.get(), data_of(label), length_of(label.size())) != 1 ||
      EVP_PKEY_derive(context.get(), out.data(), &size) != 1 || size != length) {
    throw std::runtime_error{"HKDF-SHA-256 failed"};
  }
  return out;
}

std::string seal(std::span<const std::uint8_t> key, std::span<const std::uint8_t> nonce,
                 std::string_view header, std::string_view plain) {
  const Cipher context = cipher_for(key, nonce, true);
  std::string out(plain.size() + kSealTagBytes, '\0');
  auto *bytes = reinterpret_cast<unsigned char *>(out.data());
  int written = 0;
  int ignored = 0;
  if (!context ||
      EVP_EncryptUpdate(context.get(), nullptr, &ignored, data_of(header),
                        length_of(header.size())) != 1 ||
      EVP_EncryptUpdate(context.get(), bytes, &written, data_of(plain), length_of(plain.size())) !=
          1 ||
      EVP_EncryptFinal_ex(context.get(), bytes + written, &ignored) != 1 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_GET_TAG, static_cast<int>(kSealTagBytes),
                          bytes + plain.size()) != 1) {
    throw std::runtime_error{"ChaCha20-Poly1305 sealing failed"};
  }
  return out;
}

std::optional<std::string> open(std::span<const std::uint8_t> key,
                                std::span<const std::uint8_t> nonce, std::string_view header,
                                std::string_view sealed) {
  if (sealed.size() < kSealTagBytes) {
    return std::nullopt;
  }
  const Cipher context = cipher_for(key, nonce, false);
  const std::size_t plain_size = sealed.size() - kSealTagBytes;
  std::string out(plain_size, '\0');
  auto *bytes = reinterpret_cast<unsigned char *>(out.data());
  std::array<unsigned char, kSealTagBytes> tag{};
  std::copy_n(data_of(sealed) + plain_size, kSealTagBytes, tag.begin());
  int written = 0;
  int ignored = 0;
  if (!context ||
      EVP_DecryptUpdate(context.get(), nullptr, &ignored, data_of(header),
                        length_of(header.size())) != 1 ||
      EVP_DecryptUpdate(context.get(), bytes, &written, data_of(sealed), length_of(plain_size)) !=
          1 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_SET_TAG, static_cast<int>(kSealTagBytes),
                          tag.data()) != 1 ||
      EVP_DecryptFinal_ex(context.get(), bytes + written, &ignored) != 1) {
    return std::nullopt;
  }
  return out;
}

bool same_bytes(std::span<const std::uint8_t> first,
                std::span<const std::uint8_t> second) noexcept {
  return first.size() == second.size() &&
         CRYPTO_memcmp(first.data(), second.data(), first.size()) == 0;
}

void wipe(std::span<std::uint8_t> bytes) noexcept {
  OPENSSL_cleanse(bytes.data(), bytes.size());
}

struct KeyPair::Key {
  std::unique_ptr<EVP_PKEY, FreeKey> key;
};

KeyPair::KeyPair() : KeyPair{random_bytes(kKeyBytes)} {
}

KeyPair::KeyPair(Bytes secret) : key_{std::make_unique<Key>()} {
  if (secret.size() != kKeyBytes) {
    wipe(secret);
    throw std::invalid_argument{"an X25519 secret is 32 bytes"};
  }
  key_->key.reset(
      EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, secret.data(), secret.size()));
  wipe(secret);
  std::size_t size = public_key_.size();
  if (!key_->key || EVP_PKEY_get_raw_public_key(key_->key.get(), public_key_.data(), &size) != 1 ||
      size != public_key_.size()) {
    throw std::runtime_error{"X25519 key unavailable"};
  }
}

KeyPair::~KeyPair() = default;

std::optional<Bytes> KeyPair::agree(std::span<const std::uint8_t> peer) const {
  if (peer.size() != kKeyBytes) {
    return std::nullopt;
  }
  const std::unique_ptr<EVP_PKEY, FreeKey> other{
      EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peer.data(), peer.size())};
  const KeyContext context{EVP_PKEY_CTX_new(key_->key.get(), nullptr)};
  Bytes shared(kKeyBytes);
  std::size_t size = shared.size();
  if (!other || !context || EVP_PKEY_derive_init(context.get()) != 1 ||
      EVP_PKEY_derive_set_peer(context.get(), other.get()) != 1 ||
      EVP_PKEY_derive(context.get(), shared.data(), &size) != 1 || size != kKeyBytes) {
    return std::nullopt;
  }
  const Bytes zeros(kKeyBytes);
  if (same_bytes(shared, zeros)) {
    return std::nullopt;
  }
  return shared;
}

} // namespace lucent::p2p::crypto

#include "lucent/p2p/crypto.h"
#include "lucent/p2p/packet_seal.h"

#include <cassert>
#include <cstdio>
#include <string>

namespace {

using namespace lucent::p2p;

void seal_opens_only_unchanged_under_its_key() {
  const crypto::Bytes key = crypto::random_bytes(crypto::kKeyBytes);
  const crypto::Bytes nonce = crypto::random_bytes(crypto::kSealNonceBytes);
  const std::string sealed = crypto::seal(key, nonce, "head", "plain text");
  assert(sealed.size() == 10 + crypto::kSealTagBytes);
  assert(crypto::open(key, nonce, "head", sealed) == "plain text");
  assert(!crypto::open(key, nonce, "HEAD", sealed));
  std::string changed = sealed;
  changed[0] = static_cast<char>(changed[0] ^ 1);
  assert(!crypto::open(key, nonce, "head", changed));
  const crypto::Bytes other = crypto::random_bytes(crypto::kKeyBytes);
  assert(!crypto::open(other, nonce, "head", sealed));
}

void x25519_agrees_and_refuses_bad_keys() {
  const crypto::KeyPair a;
  const crypto::KeyPair b;
  const auto ab = a.agree(b.public_key());
  const auto ba = b.agree(a.public_key());
  assert(ab && ba && *ab == *ba);
  const crypto::Bytes zeros(crypto::kKeyBytes);
  assert(!a.agree(zeros));
  assert(!a.agree(crypto::Bytes(31)));
}

void hkdf_matches_rfc5869_case_1() {
  const crypto::Bytes ikm(22, 0x0b);
  const crypto::Bytes salt{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                           0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c};
  const std::string info{"\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9"};
  const crypto::Bytes okm = crypto::hkdf_sha256(ikm, salt, info, 42);
  assert(okm[0] == 0x3c && okm[1] == 0xb2 && okm[41] == 0x65);
}

void opener_takes_each_counter_once() {
  const crypto::Bytes key = crypto::random_bytes(crypto::kKeyBytes);
  PacketSeal seal{key};
  PacketOpener opener{key};
  const std::string first = seal.seal(7, "one");
  const std::string second = seal.seal(7, "two");
  const auto opened = opener.open(second);
  assert(opened && opened->kind == 7 && opened->payload == "two" && opened->counter == 1);
  assert(opener.open(first));
  assert(!opener.open(first));
  assert(!opener.open(second));
  PacketOpener stranger{crypto::random_bytes(crypto::kKeyBytes)};
  assert(!stranger.open(seal.seal(7, "three")));
}

} // namespace

int main() {
  seal_opens_only_unchanged_under_its_key();
  x25519_agrees_and_refuses_bad_keys();
  hkdf_matches_rfc5869_case_1();
  opener_takes_each_counter_once();
  std::puts("lucent p2p crypto tests passed");
  return 0;
}

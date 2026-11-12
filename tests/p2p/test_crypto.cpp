#include "lucent/p2p/crypto.h"
#include "lucent/p2p/packet_seal.h"
#include "lucent/text.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <string_view>

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

crypto::Bytes bytes(std::string_view hex) {
  return lucent::text::from_hex(hex).value_or(crypto::Bytes{});
}

void x25519_matches_rfc7748_section_6_1() {
  const crypto::KeyPair alice{
      bytes("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")};
  const crypto::KeyPair bob{
      bytes("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb")};
  assert(lucent::text::hex(alice.public_key()) ==
         "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
  assert(lucent::text::hex(bob.public_key()) ==
         "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
  const auto shared = alice.agree(bob.public_key());
  assert(shared && lucent::text::hex(*shared) ==
                       "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
}

void seal_matches_rfc8439_section_2_8_2() {
  const crypto::Bytes key =
      bytes("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
  const crypto::Bytes nonce = bytes("070000004041424344454647");
  const crypto::Bytes aad = bytes("50515253c0c1c2c3c4c5c6c7");
  const std::string plain = "Ladies and Gentlemen of the class of '99: If I could offer you only "
                            "one tip for the future, sunscreen would be it.";
  const std::string sealed = crypto::seal(key, nonce, crypto::text_of(aad), plain);
  const std::string hex = lucent::text::hex(crypto::bytes_of(sealed));
  assert(hex.starts_with("d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"));
  assert(hex.ends_with("1ae10b594f09e26a7e902ecbd0600691"));
  assert(crypto::open(key, nonce, crypto::text_of(aad), sealed) == plain);
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
  x25519_matches_rfc7748_section_6_1();
  seal_matches_rfc8439_section_2_8_2();
  opener_takes_each_counter_once();
  std::puts("lucent p2p crypto tests passed");
  return 0;
}

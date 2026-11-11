#include "lucent/p2p/descriptor.h"
#include "lucent/p2p/handshake.h"
#include "lucent/p2p/reliable_link.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "lucent/p2p/packet_seal.h"
#include "loopback_paths.h"

namespace {

using namespace lucent::p2p;
using namespace std::chrono_literals;

constexpr std::string_view kLabel = "lucent-test";

LinkCaps caps() {
  LinkCaps limits{};
  limits[1] = 64;
  return limits;
}

void friend_and_host_find_each_other_and_agree_keys() {
  auto host_path = test::bound_path();
  auto friend_path = test::bound_path();
  const crypto::Bytes secret = crypto::random_bytes(16);
  Handshake host{*host_path, Role::Host, secret, std::string{kLabel}};
  Handshake guest{*friend_path, Role::Friend, secret, std::string{kLabel}};
  guest.aim({test::loopback_of(*host_path)});
  for (int round = 0; round < 100 && !guest.found(); ++round) {
    guest.step(5ms);
    host.step(5ms);
  }
  assert(guest.found() && !host.found());
  // The host takes the friend only when a datagram opens under the session keys.
  const Found &found = *guest.found();
  const crypto::Bytes send = found.keys.key(kLinkPurpose, Role::Friend);
  const crypto::Bytes receive = found.keys.key(kLinkPurpose, Role::Host);
  ReliableLink link{*friend_path, found.peer, {send, receive}, caps()};
  link.send(1, "hello");
  for (int round = 0; round < 100 && !host.found(); ++round) {
    link.pump(5ms);
    host.step(5ms);
  }
  assert(host.found());
  assert(host.found()->keys == found.keys);
  assert(host.found()->peer == test::loopback_of(*friend_path));
  // No key serves two senders or two purposes.
  assert(found.keys.key("game", Role::Host) != found.keys.key("game", Role::Friend));
  assert(found.keys.key(kLinkPurpose, Role::Host) != found.keys.key("game", Role::Host));
}

void run(Handshake &a, Handshake &b, int rounds = 40) {
  for (int round = 0; round < rounds && !(a.found() && b.found()); ++round) {
    a.step(5ms);
    b.step(5ms);
  }
}

// Whatever `path` has, waiting up to `wait` for each.
std::vector<std::string> heard(Path &path, std::chrono::milliseconds wait) {
  std::vector<std::string> datagrams;
  std::vector<char> buffer(lucent::net::kLargestDatagram);
  PathAddress from;
  for (auto length = path.receive(buffer, from, wait); length;
       length = path.receive(buffer, from, wait)) {
    datagrams.emplace_back(buffer.data(), *length);
  }
  return datagrams;
}

// A datagram sealed under `sender`'s link key, as a reliable link's first one is.
std::string sealed_by(const Found &found, Role sender) {
  PacketSeal seal{found.keys.key(kLinkPurpose, sender)};
  return seal.seal(ReliableLink::kSealKind, "ping");
}

void a_knock_reaches_a_friend_the_host_can_only_dial() {
  auto host_path = test::bound_path();
  auto friend_path = test::bound_path();
  const crypto::Bytes secret = crypto::random_bytes(kSecretBytes);
  Handshake host{*host_path, Role::Host, secret, std::string{kLabel}};
  Handshake guest{*friend_path, Role::Friend, secret, std::string{kLabel}};
  host.aim({test::loopback_of(*friend_path)});
  run(host, guest);
  assert(guest.found());
}

void a_friend_kept_near_answers_a_knock_in_full() {
  auto host_path = test::bound_path();
  auto friend_path = test::bound_path();
  const crypto::Bytes secret = crypto::random_bytes(kSecretBytes);
  Handshake host{*host_path, Role::Host, secret, std::string{kLabel}};
  Handshake guest{*friend_path, Role::Friend, secret, std::string{kLabel}};
  guest.aim({test::loopback_of(*host_path)});
  guest.keep_near();
  host.aim({test::loopback_of(*friend_path)});
  run(host, guest);
  assert(guest.found());
}

void forgeries_are_dropped_unanswered() {
  auto host_path = test::bound_path();
  auto attacker = test::bound_path();
  Handshake host{*host_path, Role::Host, crypto::random_bytes(kSecretBytes), std::string{kLabel}};
  const PathAddress target = test::loopback_of(*host_path);
  attacker->send(target, std::string(61, 'x'));
  std::string looks_like_hello(61, '\0');
  looks_like_hello[0] = static_cast<char>(kHelloKind);
  attacker->send(target, looks_like_hello);
  std::string looks_like_sealed(64, '\0');
  looks_like_sealed[0] = static_cast<char>(kSealVersion);
  looks_like_sealed[1] = static_cast<char>(ReliableLink::kSealKind);
  attacker->send(target, looks_like_sealed);
  attacker->send(target, "");
  for (int round = 0; round < 5; ++round) {
    host.step(5ms);
  }
  assert(host.dropped() == 4);
  assert(heard(*attacker, 20ms).empty());
  assert(!host.found());
}

void replays_get_nowhere() {
  auto host_path = test::bound_path();
  auto tap = test::bound_path();
  auto friend_path = test::bound_path();
  auto attacker = test::bound_path();
  const crypto::Bytes secret = crypto::random_bytes(kSecretBytes);
  Handshake host{*host_path, Role::Host, secret, std::string{kLabel}};
  Handshake guest{*friend_path, Role::Friend, secret, std::string{kLabel}};
  guest.aim({test::loopback_of(*tap)});
  guest.step(1ms);
  const std::vector<std::string> hellos = heard(*tap, 20ms);
  assert(!hellos.empty());

  // A heard hello gets its welcome back, which is no use without the friend's key.
  attacker->send(test::loopback_of(*host_path), hellos.front());
  host.step(5ms);
  assert(heard(*attacker, 20ms).size() == 1);
  assert(!host.found());

  // The real friend finishes and the code is spent.
  friend_path->send(test::loopback_of(*host_path), "");
  guest.aim({test::loopback_of(*host_path)});
  run(host, guest);
  assert(guest.found());
  const std::string proof = sealed_by(*guest.found(), Role::Friend);
  attacker->send(test::loopback_of(*host_path), proof);
  run(host, guest);
  // The first to prove with the friend's own datagram wins the address.
  assert(host.found() && host.found()->peer == test::loopback_of(*attacker));
  const std::uint64_t dropped = host.dropped();
  attacker->send(test::loopback_of(*host_path), proof);
  host.step(5ms);
  assert(host.dropped() == dropped);
}

void a_datagram_under_the_hosts_own_key_proves_no_friend() {
  auto host_path = test::bound_path();
  auto friend_path = test::bound_path();
  auto attacker = test::bound_path();
  const crypto::Bytes secret = crypto::random_bytes(kSecretBytes);
  Handshake host{*host_path, Role::Host, secret, std::string{kLabel}};
  Handshake guest{*friend_path, Role::Friend, secret, std::string{kLabel}};
  guest.aim({test::loopback_of(*host_path)});
  run(host, guest);
  assert(guest.found());
  attacker->send(test::loopback_of(*host_path), sealed_by(*guest.found(), Role::Host));
  host.step(5ms);
  assert(!host.found());
}

void a_wrong_secret_is_never_taken() {
  auto host_path = test::bound_path();
  auto friend_path = test::bound_path();
  Handshake host{*host_path, Role::Host, crypto::random_bytes(16), std::string{kLabel}};
  Handshake guest{*friend_path, Role::Friend, crypto::random_bytes(16), std::string{kLabel}};
  guest.aim({test::loopback_of(*host_path)});
  host.aim({test::loopback_of(*friend_path)});
  for (int round = 0; round < 30; ++round) {
    guest.step(5ms);
    host.step(5ms);
  }
  assert(!guest.found() && !host.found());
  // Both drop what the other sends.
  assert(host.dropped() > 0 && guest.dropped() > 0);
}

void another_application_label_is_never_taken() {
  auto host_path = test::bound_path();
  auto friend_path = test::bound_path();
  const crypto::Bytes secret = crypto::random_bytes(16);
  Handshake host{*host_path, Role::Host, secret, "one-app"};
  Handshake guest{*friend_path, Role::Friend, secret, "another-app"};
  guest.aim({test::loopback_of(*host_path)});
  for (int round = 0; round < 20; ++round) {
    guest.step(5ms);
    host.step(5ms);
  }
  assert(!guest.found() && host.dropped() > 0);
}

} // namespace

int main() {
  friend_and_host_find_each_other_and_agree_keys();
  a_wrong_secret_is_never_taken();
  another_application_label_is_never_taken();
  a_knock_reaches_a_friend_the_host_can_only_dial();
  a_friend_kept_near_answers_a_knock_in_full();
  forgeries_are_dropped_unanswered();
  replays_get_nowhere();
  a_datagram_under_the_hosts_own_key_proves_no_friend();
  std::puts("lucent p2p handshake tests passed");
  return 0;
}

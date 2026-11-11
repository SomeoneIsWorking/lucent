#include "lucent/p2p/handshake.h"
#include "lucent/p2p/reliable_link.h"

#include <cassert>
#include <chrono>
#include <cstdio>

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
}

void a_wrong_secret_is_never_taken() {
  auto host_path = test::bound_path();
  auto friend_path = test::bound_path();
  Handshake host{*host_path, Role::Host, crypto::random_bytes(16), std::string{kLabel}};
  Handshake guest{*friend_path, Role::Friend, crypto::random_bytes(16), std::string{kLabel}};
  guest.aim({test::loopback_of(*host_path)});
  for (int round = 0; round < 20; ++round) {
    guest.step(5ms);
    host.step(5ms);
  }
  assert(!guest.found() && !host.found());
  assert(host.dropped() > 0);
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
  std::puts("lucent p2p handshake tests passed");
  return 0;
}

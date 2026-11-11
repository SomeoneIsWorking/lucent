#include "lucent/p2p/connector.h"
#include "lucent/p2p/reliable_link.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {

using namespace lucent::p2p;
using namespace std::chrono_literals;

ConnectorConfig loopback_config() {
  ConnectorConfig config;
  config.direct.stun_servers = "";
  config.direct.router = false;
  lucent::net::Ipv4Interface lab;
  lab.name = "lab0";
  lab.address = 0x7F000001u;
  lab.netmask = 0xFF000000u;
  lab.up = true;
  lab.running = true;
  config.direct.interfaces = std::vector<lucent::net::Ipv4Interface>{lab};
  config.prefixes = {"TST", "TSR"};
  config.label = "lucent-test";
  return config;
}

template <class Done> bool wait_until(Done done) {
  for (int round = 0; round < 400; ++round) {
    if (done()) {
      return true;
    }
    std::this_thread::sleep_for(10ms);
  }
  return false;
}

void a_pasted_invite_connects_both_sides() {
  Connector host{loopback_config()};
  assert(wait_until([&] {
    return host.view().step == Connector::Step::Inviting;
  }));
  const std::string code = host.view().code;
  CodeProblem problem{};
  std::optional<Invite> invite = read_invite(code, {"TST", "TSR"}, problem);
  assert(invite && invite->descriptor.direct);
  Connector guest{loopback_config(), std::move(*invite)};
  assert(wait_until([&] {
    return guest.view().step == Connector::Step::Connected;
  }));
  std::unique_ptr<Connection> friend_side = guest.take();
  assert(friend_side && friend_side->role == Role::Friend);
  LinkCaps caps{};
  caps[1] = 16;
  const crypto::Bytes send = friend_side->keys.key(kLinkPurpose, Role::Friend);
  const crypto::Bytes receive = friend_side->keys.key(kLinkPurpose, Role::Host);
  ReliableLink link{*friend_side->path, friend_side->peer, {send, receive}, caps};
  link.send(1, "hi");
  assert(wait_until([&] {
    link.pump(1ms);
    return host.view().step == Connector::Step::Connected;
  }));
  std::unique_ptr<Connection> host_side = host.take();
  assert(host_side && host_side->keys == friend_side->keys);
  assert(!host.take());
}

void a_host_whose_code_lapses_says_so() {
  ConnectorConfig config = loopback_config();
  config.patience.invite = 1s;
  Connector host{config};
  assert(wait_until([&] {
    return host.view().step == Connector::Step::Failed;
  }));
  assert(host.view().failure == Connector::Failure::Lapsed && host.view().code.empty());
}

} // namespace

int main() {
  a_pasted_invite_connects_both_sides();
  a_host_whose_code_lapses_says_so();
  std::puts("lucent p2p connector tests passed");
  return 0;
}

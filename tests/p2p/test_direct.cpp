#include "lucent/p2p/direct.h"

#include <cassert>
#include <cstdio>
#include <stop_token>

#include "stun_responder.h"

namespace {

using namespace lucent::p2p;
using lucent::net::Ipv4Interface;

Ipv4Interface interface(std::string name, std::uint32_t address) {
  Ipv4Interface entry;
  entry.name = std::move(name);
  entry.address = address;
  entry.up = true;
  entry.running = true;
  return entry;
}

Ipv4Interface without_carrier(Ipv4Interface entry) {
  entry.running = false;
  return entry;
}

Ipv4Interface masked(Ipv4Interface entry, std::uint32_t netmask) {
  entry.netmask = netmask;
  return entry;
}

void locals_rank_overlays_first_and_drop_what_reaches_no_one() {
  std::vector<Ipv4Interface> interfaces{
      without_carrier(masked(interface("docker0", 0xAC110001u), 0xFFFF0000u)),
      masked(interface("wlan0", 0xC0A80105u), 0xFFFFFF00u),
      masked(interface("eth1", 0x0A000005u), 0xFF000000u),
      masked(interface("eth2", 0xA9FE0101u), 0xFFFF0000u),
      masked(interface("tailscale0", 0x64400007u), 0xFFFFFFFFu),
      masked(interface("ztabcdef12", 0x0A930A01u), 0xFFFFFF00u),
  };
  Ipv4Interface loopback = masked(interface("lo", 0x7F000001u), 0xFF000000u);
  loopback.loopback = true;
  interfaces.push_back(loopback);
  const auto locals = reachable_locals(interfaces, 0xC0A80101u);
  const std::vector<std::uint32_t> expected{0x64400007u, 0x0A930A01u, 0xC0A80105u, 0x0A000005u};
  assert(locals == expected);
}

void gathering_takes_the_stun_answer_and_the_locals() {
  test::StunResponder server;
  std::string error;
  auto socket = lucent::net::UdpSocket::bind(0, error);
  assert(socket);
  DirectNetwork network;
  network.stun_servers = server.address();
  network.interfaces =
      std::vector<Ipv4Interface>{masked(interface("lab0", 0x0A000002u), 0xFFFFFF00u)};
  network.router = false;
  const DirectGathered gathered = gather_direct(std::stop_token{}, *socket, network);
  assert(gathered.reach.port == socket->port());
  assert(gathered.reach.publics.size() == 1);
  assert(gathered.reach.publics[0] == (lucent::net::Endpoint{0x7F000001u, socket->port()}));
  assert(gathered.reach.locals == std::vector<std::uint32_t>{0x0A000002u});
  assert(!gathered.reach.varying && !gathered.lease && gathered.stun_server);
}

} // namespace

int main() {
  locals_rank_overlays_first_and_drop_what_reaches_no_one();
  gathering_takes_the_stun_answer_and_the_locals();
  std::puts("lucent p2p direct tests passed");
  return 0;
}

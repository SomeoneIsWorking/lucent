#include "lucent/p2p/direct.h"
#include "lucent/worker.h"

#include <cassert>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

std::uint32_t ip(std::string_view dotted) {
  const std::optional<std::uint32_t> address = lucent::net::parse_ipv4(dotted);
  assert(address);
  return *address;
}

Ipv4Interface named(std::string_view address, std::string_view mask, std::string name) {
  return masked(interface(std::move(name), ip(address)), ip(mask));
}

void zerotier_comes_first_then_the_gateways_lan() {
  std::vector<Ipv4Interface> found{
      named("192.168.1.107", "255.255.255.0", "enp8s0"),
      without_carrier(named("172.17.0.1", "255.255.0.0", "docker0")),
      without_carrier(named("192.168.122.1", "255.255.255.0", "virbr0")),
      named("10.88.0.1", "255.255.0.0", "podman0"),
      named("169.254.3.4", "255.255.0.0", "enp9s0"),
      named("10.147.17.5", "255.255.255.0", "zt3jn4abcd"),
      named("127.0.0.1", "255.0.0.0", "lo"),
  };
  found.back().loopback = true;
  const std::vector<std::uint32_t> expected{ip("10.147.17.5"), ip("192.168.1.107"),
                                            ip("10.88.0.1")};
  assert(reachable_locals(found, ip("192.168.1.1")) == expected);
}

void tailscale_and_zerotier_share_the_front_in_order() {
  const std::vector<Ipv4Interface> found{
      named("10.0.1.2", "255.255.255.0", "eth0"),
      named("100.101.102.103", "255.192.0.0", "tailscale0"),
      named("10.147.17.5", "255.255.255.0", "zt0"),
  };
  const std::vector<std::uint32_t> expected{ip("100.101.102.103"), ip("10.147.17.5"),
                                            ip("10.0.1.2")};
  assert(reachable_locals(found, ip("10.0.1.1")) == expected);
}

void zerotier_is_known_by_its_driver_and_names() {
  const Ipv4Interface lan = named("10.0.1.2", "255.255.255.0", "eth0");
  const std::uint32_t gateway = ip("10.0.1.1");
  const std::uint32_t overlay = ip("10.147.17.5");
  // Windows' adapter, renamed, by its description.
  Ipv4Interface windows = named("10.147.17.5", "255.255.255.0", "Ethernet 3");
  windows.description = "ZeroTier Virtual Port";
  assert(reachable_locals({lan, windows}, gateway).front() == overlay);
  windows.description = "ZeroTier One Virtual Port #2";
  assert(reachable_locals({lan, windows}, gateway).front() == overlay);
  // macOS' feth pair.
  assert(
      reachable_locals({lan, named("10.147.17.5", "255.255.255.0", "feth2913")}, gateway).front() ==
      overlay);
  // A renamed Linux interface is ranked as any other, and still kept.
  const std::vector<std::uint32_t> renamed =
      reachable_locals({lan, named("10.147.17.5", "255.255.255.0", "corpnet")}, gateway);
  assert(renamed == (std::vector<std::uint32_t>{ip("10.0.1.2"), overlay}));
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
  const DirectGathered gathered = gather_direct(lucent::StopToken{}, *socket, network);
  assert(gathered.reach.port == socket->port());
  assert(gathered.reach.publics.size() == 1);
  assert(gathered.reach.publics[0] == (lucent::net::Endpoint{0x7F000001u, socket->port()}));
  assert(gathered.reach.locals == std::vector<std::uint32_t>{0x0A000002u});
  assert(!gathered.reach.varying && !gathered.lease && gathered.stun_server);
}

} // namespace

int main() {
  locals_rank_overlays_first_and_drop_what_reaches_no_one();
  zerotier_comes_first_then_the_gateways_lan();
  tailscale_and_zerotier_share_the_front_in_order();
  zerotier_is_known_by_its_driver_and_names();
  gathering_takes_the_stun_answer_and_the_locals();
  std::puts("lucent p2p direct tests passed");
  return 0;
}

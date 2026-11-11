#include "lucent/network_interfaces.h"

#include <cassert>
#include <cstdio>
#include <string>

namespace {

void host_lists_loopback_without_a_broadcast() {
  std::string error;
  const auto interfaces = lucent::net::ipv4_interfaces(error);
  assert(interfaces && error.empty());
  bool loopback = false;
  for (const lucent::net::Ipv4Interface &entry : *interfaces) {
    assert(!entry.name.empty());
    if (entry.address == 0x7f000001u) {
      loopback = entry.loopback && entry.up;
    }
    if (entry.broadcast) {
      assert(*entry.broadcast == (entry.address | ~entry.netmask));
      assert(*entry.broadcast != entry.address);
    }
  }
  assert(loopback);
  for (const std::uint32_t broadcast : lucent::net::lan_broadcasts(*interfaces)) {
    assert(broadcast != 0x7fffffffu && (broadcast >> 24u) != 127u);
  }
}

void broadcasts_skip_loopback_no_carrier_and_repeats() {
  using lucent::net::Ipv4Interface;
  const std::vector<Ipv4Interface> interfaces = {
      {.name = "lo",
       .address = 0x7f000001u,
       .netmask = 0xff000000u,
       .broadcast = 0x7fffffffu,
       .up = true,
       .running = true,
       .loopback = true},
      {.name = "eth0",
       .address = 0xc0a80105u,
       .netmask = 0xffffff00u,
       .broadcast = 0xc0a801ffu,
       .up = true,
       .running = true,
       .loopback = false},
      {.name = "eth0",
       .address = 0xc0a80106u,
       .netmask = 0xffffff00u,
       .broadcast = 0xc0a801ffu,
       .up = true,
       .running = true,
       .loopback = false},
      {.name = "tun0",
       .address = 0x0a080001u,
       .netmask = 0xffffffffu,
       .broadcast = std::nullopt,
       .up = true,
       .running = true,
       .loopback = false},
      {.name = "docker0",
       .address = 0xac110001u,
       .netmask = 0xffff0000u,
       .broadcast = 0xac11ffffu,
       .up = true,
       .running = false,
       .loopback = false},
      {.name = "zt0",
       .address = 0x0a930011u,
       .netmask = 0xffff0000u,
       .broadcast = 0x0a93ffffu,
       .up = true,
       .running = true,
       .loopback = false},
  };
  const std::vector<std::uint32_t> expected = {0xc0a801ffu, 0x0a93ffffu};
  assert(lucent::net::lan_broadcasts(interfaces) == expected);
}

} // namespace

int main() {
  host_lists_loopback_without_a_broadcast();
  broadcasts_skip_loopback_no_carrier_and_repeats();
  std::puts("network interface tests passed");
  return 0;
}

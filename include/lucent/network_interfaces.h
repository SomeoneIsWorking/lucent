#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lucent::net {

// One IPv4 address of a network interface. Addresses are in host byte order.
struct Ipv4Interface {
  // The OS interface name (Linux `zt3jn4abcd`, Windows the adapter's friendly name).
  std::string name;
  // Windows' adapter description ("ZeroTier Virtual Port"); empty elsewhere.
  std::string description;
  std::uint32_t address = 0;
  std::uint32_t netmask = 0;
  // The subnet's directed broadcast address, present only when the interface can broadcast.
  std::optional<std::uint32_t> broadcast;
  bool up = false;
  // Carrier present: a bridge with no ports is up but not running.
  bool running = false;
  bool loopback = false;
  bool multicast = false;
  bool point_to_point = false;
};

// Every IPv4 address of every interface, virtual networks (VPNs, ZeroTier, bridges) included.
// Nothing when the host refuses the query; `error` then says why. A browser page has none.
std::optional<std::vector<Ipv4Interface>> ipv4_interfaces(std::string &error);

// The distinct directed broadcast addresses of the running, non-loopback `interfaces`, in order. A
// datagram sent to each reaches every IPv4 LAN the host is on, not only the default route's.
std::vector<std::uint32_t> lan_broadcasts(const std::vector<Ipv4Interface> &interfaces);

} // namespace lucent::net

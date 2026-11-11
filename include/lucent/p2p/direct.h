#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lucent/network_interfaces.h"
#include "lucent/p2p/descriptor.h"
#include "lucent/p2p/port_lease.h"
#include "lucent/udp_socket.h"
#include "lucent/worker.h"

// The direct backend's view of where its socket can be reached: this machine's interface
// addresses, what two STUN servers saw, and a router mapping by NAT-PMP or UPnP.
namespace lucent::p2p {

// Public STUN servers of two operators, so a NAT that varies its mapping shows.
inline constexpr std::string_view kDefaultStunServers =
    "stun.l.google.com:19302,stun.cloudflare.com:3478";

// Where the direct backend looks; a test or a lab points these elsewhere.
struct DirectNetwork {
  // "host:port", comma-separated.
  std::string stun_servers{kDefaultStunServers};
  // Instead of the system's.
  std::optional<std::vector<net::Ipv4Interface>> interfaces;
  // Whether to ask the router for a port at all.
  bool router = true;
  // NAT-PMP's address; the default gateway's when not set.
  std::optional<net::Endpoint> nat_pmp;
  // Where SSDP searches go; the multicast group when not set.
  std::optional<net::Endpoint> ssdp;
};

struct DirectGathered {
  DirectReach reach;
  std::unique_ptr<PortLease> lease;
  // The STUN server that answered, to keep the NAT's mapping alive with.
  std::optional<net::Endpoint> stun_server;
};

// `interfaces` in the order a code keeps them: interfaces without a carrier and link-local
// addresses left out; Tailscale's and ZeroTier's first, as they reach past any NAT; then the one
// `gateway` is on; then the rest in the system's order.
std::vector<std::uint32_t> reachable_locals(const std::vector<net::Ipv4Interface> &interfaces,
                                            std::optional<std::uint32_t> gateway);

// Where `socket` can be reached. Bounded by a few seconds; stops early on `stop`.
DirectGathered gather_direct(const StopToken &stop, const net::UdpSocket &socket,
                             const DirectNetwork &network);

} // namespace lucent::p2p

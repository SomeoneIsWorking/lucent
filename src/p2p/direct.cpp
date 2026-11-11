#include "lucent/p2p/direct.h"

#include <algorithm>
#include <chrono>

#include "lucent/log.h"
#include "lucent/p2p/nat_pmp.h"
#include "lucent/p2p/stun.h"
#include "lucent/p2p/upnp_igd.h"

namespace lucent::p2p {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr auto kStunResend = 300ms;
constexpr auto kStunWait = 2s;
constexpr auto kReceiveWait = 50ms;
constexpr auto kSsdpWait = 1500ms;
constexpr net::Endpoint kSsdpGroup{0xEFFFFFFAu, kSsdpPort};

bool link_local(std::uint32_t address) {
  return (address >> 16u) == 0xA9FEu;
}

bool tailscale(std::uint32_t address) {
  return (address >> 22u) == (0x64400000u >> 22u);
}

bool zerotier(const net::Ipv4Interface &interface) {
  return interface.name.starts_with("zt") || interface.name.starts_with("feth") ||
         interface.description.find("ZeroTier") != std::string::npos;
}

bool on_link_with(const net::Ipv4Interface &interface, std::uint32_t address) {
  return interface.netmask != 0 &&
         (interface.address & interface.netmask) == (address & interface.netmask);
}

std::vector<net::Endpoint> servers_of(std::string_view list) {
  std::vector<net::Endpoint> servers;
  while (!list.empty()) {
    const std::size_t comma = list.find(',');
    if (const auto server = net::resolve_endpoint(list.substr(0, comma))) {
      servers.push_back(*server);
    }
    list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
  }
  return servers;
}

// The address each server saw, from one socket; nothing for one that did not answer.
std::vector<std::optional<net::Endpoint>> ask_servers(const std::stop_token &stop,
                                                      const net::UdpSocket &socket,
                                                      const std::vector<net::Endpoint> &servers) {
  std::vector<StunId> ids(servers.size());
  std::generate(ids.begin(), ids.end(), new_stun_id);
  std::vector<std::optional<net::Endpoint>> seen(servers.size());
  std::vector<char> buffer(net::kLargestDatagram);
  const auto until = Clock::now() + kStunWait;
  auto next_send = Clock::time_point{};
  while (Clock::now() < until && !stop.stop_requested() &&
         std::any_of(seen.begin(), seen.end(), [](const auto &answer) {
           return !answer.has_value();
         })) {
    if (Clock::now() >= next_send) {
      next_send = Clock::now() + kStunResend;
      for (std::size_t at = 0; at < servers.size(); ++at) {
        if (!seen[at]) {
          socket.send(servers[at], stun_request(ids[at]));
        }
      }
    }
    net::Endpoint from;
    const std::optional<std::size_t> length = socket.receive(buffer, from, kReceiveWait);
    if (!length) {
      continue;
    }
    for (std::size_t at = 0; at < servers.size(); ++at) {
      if (from == servers[at]) {
        if (const auto mapped = stun_mapped({buffer.data(), *length}, ids[at])) {
          seen[at] = mapped;
        }
      }
    }
  }
  return seen;
}

std::unique_ptr<PortLease> map_port(const std::stop_token &stop, const DirectNetwork &network,
                                    std::uint16_t port, const std::vector<std::uint32_t> &locals,
                                    std::optional<std::uint32_t> gateway) {
  std::optional<net::Endpoint> nat_pmp = network.nat_pmp;
  if (!nat_pmp && gateway) {
    nat_pmp = net::Endpoint{*gateway, kNatPmpPort};
  }
  if (nat_pmp) {
    std::string error;
    std::unique_ptr<NatPmp> mapper = NatPmp::create(*nat_pmp, error);
    if (!mapper) {
      lucent::log(Level::Warn, "p2p", "no NAT-PMP socket: " + error);
    } else if (const std::optional<Mapping> mapping = mapper->open(port, kAskedLease)) {
      lucent::log(Level::Info, "p2p", "NAT-PMP forwards a port");
      return std::make_unique<PortLease>(std::move(mapper), port, *mapping);
    }
  }
  if (locals.empty()) {
    return nullptr;
  }
  std::unique_ptr<UpnpIgd> igd =
      UpnpIgd::discover(stop, network.ssdp.value_or(kSsdpGroup), locals.front(), kSsdpWait);
  if (igd) {
    if (const std::optional<Mapping> mapping = igd->open(port, kAskedLease)) {
      lucent::log(Level::Info, "p2p", "UPnP forwards a port");
      return std::make_unique<PortLease>(std::move(igd), port, *mapping);
    }
  }
  return nullptr;
}

} // namespace

std::vector<std::uint32_t> reachable_locals(const std::vector<net::Ipv4Interface> &interfaces,
                                            std::optional<std::uint32_t> gateway) {
  std::vector<std::uint32_t> first;
  std::vector<std::uint32_t> routed;
  std::vector<std::uint32_t> rest;
  for (const net::Ipv4Interface &interface : interfaces) {
    const std::uint32_t address = interface.address;
    if (!interface.up || !interface.running || interface.loopback || address == 0 ||
        link_local(address)) {
      continue;
    }
    if (tailscale(address) || zerotier(interface)) {
      first.push_back(address);
    } else if (gateway && on_link_with(interface, *gateway)) {
      routed.push_back(address);
    } else {
      rest.push_back(address);
    }
  }
  first.insert(first.end(), routed.begin(), routed.end());
  first.insert(first.end(), rest.begin(), rest.end());
  return first;
}

DirectGathered gather_direct(const std::stop_token &stop, const net::UdpSocket &socket,
                             const DirectNetwork &network) {
  DirectGathered gathered;
  gathered.reach.port = socket.port();
  std::optional<std::vector<net::Ipv4Interface>> listed = network.interfaces;
  if (!listed) {
    std::string why;
    listed = net::ipv4_interfaces(why);
    if (!listed) {
      lucent::log(Level::Warn, "p2p", "this machine's interfaces are unknown: " + why);
      listed.emplace();
    }
  }
  const std::optional<std::uint32_t> gateway = net::default_gateway();
  std::vector<std::uint32_t> locals = reachable_locals(*listed, gateway);
  gathered.reach.locals = locals;
  if (gathered.reach.locals.size() > kMostAddresses) {
    gathered.reach.locals.resize(kMostAddresses);
  }
  const std::vector<net::Endpoint> servers = servers_of(network.stun_servers);
  const auto seen = ask_servers(stop, socket, servers);
  std::vector<net::Endpoint> answered;
  for (std::size_t at = 0; at < seen.size(); ++at) {
    if (const std::optional<net::Endpoint> &mapped = seen[at]) {
      gathered.stun_server = gathered.stun_server.value_or(servers[at]);
      if (std::find(answered.begin(), answered.end(), *mapped) == answered.end()) {
        answered.push_back(*mapped);
      }
    }
  }
  const bool varying = answered.size() > 1;
  if (!answered.empty()) {
    gathered.reach.publics.push_back(answered.front());
  }
  if (network.router) {
    // A mapping points at the interface the router is on.
    std::erase_if(locals, [&listed, gateway](std::uint32_t address) {
      return gateway && std::none_of(listed->begin(), listed->end(),
                                     [address, gateway](const net::Ipv4Interface &interface) {
                                       return interface.address == address &&
                                              on_link_with(interface, *gateway);
                                     });
    });
    gathered.lease = map_port(stop, network, socket.port(), locals, gateway);
    if (gathered.lease) {
      gathered.reach.publics.insert(gathered.reach.publics.begin(), gathered.lease->external());
    }
  }
  gathered.reach.varying = varying && !gathered.lease;
  std::string named;
  for (const std::uint32_t address : gathered.reach.locals) {
    named += (named.empty() ? "" : " ") + net::format_ipv4(address);
  }
  lucent::log(Level::Info, "p2p",
              std::to_string(gathered.reach.publics.size()) + " public addresses, " +
                  std::to_string(gathered.reach.locals.size()) + " local (" + named +
                  "), mapping " + (varying ? "varies by destination" : "steady"));
  return gathered;
}

} // namespace lucent::p2p

// Two virtual LANs over loopback paths: guest datagrams cross both ways as the friends' addresses,
// one loopback socket stands for each friend and port, an app-chosen address is kept, and a
// datagram under other keys never reaches the game.
#include "lucent/p2p/virtual_lan.h"

#include "lucent/p2p/path.h"
#include "lucent/udp_socket.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using lucent::net::Endpoint;
using lucent::net::UdpSocket;
using lucent::p2p::Connection;
using lucent::p2p::PathAddress;
using lucent::p2p::Role;
using lucent::p2p::SessionKeys;
using lucent::p2p::UdpPath;
using lucent::p2p::VirtualLan;

constexpr std::uint32_t kLoopback = 0x7F000001U;

UdpSocket bound() {
  std::string error;
  std::optional<UdpSocket> socket = UdpSocket::bind(0, error);
  assert(socket);
  return std::move(*socket);
}

std::unique_ptr<Connection> connection(Role role, UdpSocket socket, std::uint16_t peer_port,
                                       const SessionKeys &keys) {
  auto out = std::make_unique<Connection>(Connection{role, nullptr, {}, keys, nullptr});
  out->path = std::make_unique<UdpPath>(std::move(socket));
  out->peer = PathAddress::from(Endpoint{kLoopback, peer_port});
  return out;
}

struct Received {
  std::string bytes;
  Endpoint from;
};

Received receive(const UdpSocket &socket) {
  std::array<char, 2048> buffer{};
  Endpoint from;
  const std::optional<std::size_t> length =
      socket.receive(buffer, from, std::chrono::milliseconds{2000});
  assert(length);
  return {std::string(buffer.data(), *length), from};
}

void guest_datagrams_cross_both_ways_as_friend_addresses() {
  const SessionKeys keys({1, 2, 3, 4}, {5, 6}, "lucent-test");
  UdpSocket host_path = bound();
  UdpSocket friend_path = bound();
  const std::uint16_t host_port = host_path.port();
  const std::uint16_t friend_port = friend_path.port();
  VirtualLan host;
  VirtualLan guest;
  const std::optional<std::uint32_t> friend_on_host =
      host.add(connection(Role::Host, std::move(host_path), friend_port, keys));
  const std::optional<std::uint32_t> host_on_friend =
      guest.add(connection(Role::Friend, std::move(friend_path), host_port, keys));
  assert(friend_on_host == lucent::p2p::kVirtualLanFirstAddress);
  assert(host_on_friend == lucent::p2p::kVirtualLanFirstAddress);
  assert(host.is_friend(*friend_on_host) && !host.is_friend(*friend_on_host + 1));

  // The friend's game socket, and the host's.
  const UdpSocket friend_game = bound();
  const UdpSocket host_game = bound();

  assert(host.send({*friend_on_host, friend_game.port()}, host_game.port(), "query"));
  const Received query = receive(friend_game);
  assert(query.bytes == "query");
  assert(query.from.address == kLoopback);
  const std::optional<Endpoint> sender = guest.source_of(query.from.port);
  assert(sender && sender->address == *host_on_friend && sender->port == host_game.port());

  // The friend answers the address it was shown.
  assert(guest.send(*sender, friend_game.port(), "answer"));
  const Received answer = receive(host_game);
  assert(answer.bytes == "answer");
  const std::optional<Endpoint> replier = host.source_of(answer.from.port);
  assert(replier && replier->address == *friend_on_host && replier->port == friend_game.port());

  // One proxy per friend and port: a second datagram arrives from the same loopback port.
  assert(host.broadcast(friend_game.port(), host_game.port(), "again") == 1);
  const Received again = receive(friend_game);
  assert(again.bytes == "again" && again.from.port == query.from.port);

  assert(!host.send({*friend_on_host + 1, friend_game.port()}, host_game.port(), "nobody"));
}

void an_app_chosen_address_is_kept_and_not_shared() {
  const SessionKeys keys({1, 2, 3, 4}, {5, 6}, "lucent-test");
  UdpSocket first = bound();
  UdpSocket second = bound();
  VirtualLan lan;
  constexpr std::uint32_t seat = lucent::p2p::kVirtualLanFirstAddress + 7;
  assert(lan.add(connection(Role::Host, std::move(first), 9, keys), seat) == seat);
  assert(!lan.add(connection(Role::Host, std::move(second), 9, keys), seat));
  assert(lan.friends() == std::vector<std::uint32_t>{seat});
}

void a_datagram_under_other_keys_is_dropped() {
  UdpSocket host_path = bound();
  UdpSocket friend_path = bound();
  const std::uint16_t host_port = host_path.port();
  const std::uint16_t friend_port = friend_path.port();
  VirtualLan host;
  VirtualLan guest;
  host.add(connection(Role::Host, std::move(host_path), friend_port,
                      SessionKeys({1, 2, 3}, {4}, "lucent-test")));
  guest.add(connection(Role::Friend, std::move(friend_path), host_port,
                       SessionKeys({9, 9, 9}, {4}, "lucent-test")));
  const UdpSocket friend_game = bound();
  assert(host.send({lucent::p2p::kVirtualLanFirstAddress, friend_game.port()}, 4000, "forged"));
  std::array<char, 64> buffer{};
  Endpoint from;
  assert(!friend_game.receive(buffer, from, std::chrono::milliseconds{500}));
}

} // namespace

int main() {
  guest_datagrams_cross_both_ways_as_friend_addresses();
  an_app_chosen_address_is_kept_and_not_shared();
  a_datagram_under_other_keys_is_dropped();
  std::puts("a virtual LAN carries guest datagrams as friends' addresses and drops forged ones");
  return 0;
}

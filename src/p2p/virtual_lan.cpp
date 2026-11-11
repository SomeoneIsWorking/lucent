#include "lucent/p2p/virtual_lan.h"

#include "lucent/log.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <string>

namespace lucent::p2p {
namespace {

constexpr std::uint8_t kGuestDatagram = 0x47;
constexpr std::uint8_t kKeepalive = 0x4B;
// The LAN owns its connection, so it seals under the link purpose: the friend's first datagram
// completes the host's meeting.
constexpr std::string_view kPurpose = kLinkPurpose;
// Each side sends at least this often, so a silent game keeps its NAT mappings.
constexpr std::chrono::seconds kKeepaliveEvery{1};
// The source and destination ports before the game's bytes.
constexpr std::size_t kPortsBytes = 4;
constexpr std::uint32_t kLoopback = 0x7F000001U;
constexpr std::chrono::milliseconds kReadWait{100};

Role other(Role role) {
  return role == Role::Host ? Role::Friend : Role::Host;
}

void put_port(std::string &out, std::uint16_t port) {
  out.push_back(static_cast<char>(port >> 8U));
  out.push_back(static_cast<char>(port & 0xFFU));
}

std::uint16_t port_at(std::string_view in, std::size_t at) {
  return static_cast<std::uint16_t>((static_cast<unsigned char>(in[at]) << 8U) |
                                    static_cast<unsigned char>(in[at + 1]));
}

} // namespace

VirtualLan::~VirtualLan() {
  // Readers stop before the connections they read from go.
  for (const std::unique_ptr<Friend> &peer : friends_) {
    peer->reader.request_stop();
  }
  for (const std::unique_ptr<Friend> &peer : friends_) {
    if (peer->reader.joinable()) {
      peer->reader.join();
    }
  }
}

std::optional<std::uint32_t> VirtualLan::add(std::unique_ptr<Connection> connection,
                                             std::optional<std::uint32_t> address) {
  const std::lock_guard lock{mutex_};
  if (friends_.size() >= kVirtualLanMaxFriends) {
    log(Level::Error, "lan", "no room for another friend");
    return std::nullopt;
  }
  std::uint32_t chosen = address.value_or(kVirtualLanFirstAddress);
  if (!address) {
    while (find(chosen) != nullptr) {
      ++chosen;
    }
  } else if (find(chosen) != nullptr) {
    log(Level::Error, "lan", net::format_ipv4(chosen) + " is already a friend's");
    return std::nullopt;
  }
  auto peer = std::make_unique<Friend>();
  peer->address = chosen;
  const Role role = connection->role;
  peer->seal = std::make_unique<PacketSeal>(connection->keys.key(kPurpose, role));
  auto opener = std::make_unique<PacketOpener>(connection->keys.key(kPurpose, other(role)));
  peer->connection = std::move(connection);
  Friend &added = *peer;
  peer->reader = Worker{[this, &added, opener = std::move(opener)](const StopToken &stop) mutable {
    read(stop, added, std::move(opener));
  }};
  friends_.push_back(std::move(peer));
  log(Level::Info, "lan",
      "a friend joined over " + std::string{name_of(added.connection->path->backend())} + " as " +
          net::format_ipv4(chosen));
  return chosen;
}

VirtualLan::Friend *VirtualLan::find(std::uint32_t address) const {
  const auto found = std::find_if(friends_.begin(), friends_.end(),
                                  [address](const std::unique_ptr<Friend> &peer) {
                                    return peer->address == address;
                                  });
  return found != friends_.end() ? found->get() : nullptr;
}

bool VirtualLan::is_friend(std::uint32_t address) const {
  const std::lock_guard lock{mutex_};
  return find(address) != nullptr;
}

std::vector<std::uint32_t> VirtualLan::friends() const {
  const std::lock_guard lock{mutex_};
  std::vector<std::uint32_t> out;
  out.reserve(friends_.size());
  for (const std::unique_ptr<Friend> &peer : friends_) {
    out.push_back(peer->address);
  }
  return out;
}

bool VirtualLan::send(const net::Endpoint &to, std::uint16_t from_port, std::string_view bytes) {
  Friend *peer = nullptr;
  {
    const std::lock_guard lock{mutex_};
    peer = find(to.address);
  }
  if (peer == nullptr) {
    return false;
  }
  std::string payload;
  payload.reserve(kPortsBytes + bytes.size());
  put_port(payload, from_port);
  put_port(payload, to.port);
  payload.append(bytes);
  const std::lock_guard lock{peer->send_mutex};
  return peer->connection->path->send(peer->connection->peer,
                                      peer->seal->seal(kGuestDatagram, payload));
}

std::size_t VirtualLan::broadcast(std::uint16_t to_port, std::uint16_t from_port,
                                  std::string_view bytes) {
  std::size_t taken = 0;
  for (const std::uint32_t address : friends()) {
    if (send({address, to_port}, from_port, bytes)) {
      ++taken;
    }
  }
  return taken;
}

std::optional<net::Endpoint> VirtualLan::source_of(std::uint16_t loopback_port) const {
  const std::lock_guard lock{mutex_};
  const auto found = proxies_.find(loopback_port);
  if (found == proxies_.end()) {
    return std::nullopt;
  }
  return found->second.source;
}

void VirtualLan::read(const StopToken &stop, Friend &peer, std::unique_ptr<PacketOpener> opener) {
  std::array<char, net::kLargestDatagram> buffer{};
  Path &path = *peer.connection->path;
  std::chrono::steady_clock::time_point next_keepalive{};
  while (!stop.stop_requested()) {
    if (const auto now = std::chrono::steady_clock::now(); now >= next_keepalive) {
      const std::lock_guard lock{peer.send_mutex};
      path.send(peer.connection->peer, peer.seal->seal(kKeepalive, {}));
      next_keepalive = now + kKeepaliveEvery;
    }
    PathAddress from;
    const std::optional<std::size_t> length = path.receive(buffer, from, kReadWait);
    if (!length || !(from == peer.connection->peer)) {
      continue;
    }
    const std::optional<Opened> opened = opener->open(std::string_view{buffer.data(), *length});
    if (!opened || opened->kind != kGuestDatagram || opened->payload.size() < kPortsBytes) {
      continue;
    }
    deliver({peer.address, port_at(opened->payload, 0)}, port_at(opened->payload, 2),
            std::string_view{opened->payload}.substr(kPortsBytes));
  }
}

void VirtualLan::deliver(const net::Endpoint &from, std::uint16_t to_port,
                         std::string_view payload) {
  const std::lock_guard lock{mutex_};
  auto known = proxy_of_.find({from.address, from.port});
  if (known == proxy_of_.end()) {
    std::string error;
    std::optional<net::UdpSocket> socket = net::UdpSocket::bind(0, error);
    if (!socket) {
      log(Level::Error, "lan", "no loopback socket for a friend's datagram: " + error);
      return;
    }
    const std::uint16_t port = socket->port();
    proxies_.emplace(port, Proxy{from, std::move(*socket)});
    known = proxy_of_.emplace(std::pair{from.address, from.port}, port).first;
  }
  proxies_.at(known->second).socket.send({kLoopback, to_port}, payload);
}

} // namespace lucent::p2p

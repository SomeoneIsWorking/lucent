#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "lucent/ipv4.h"
#include "lucent/p2p/connector.h"
#include "lucent/p2p/packet_seal.h"
#include "lucent/udp_socket.h"
#include "lucent/worker.h"

namespace lucent::p2p {

// Friends' addresses on the virtual LAN start here: 10.254.0.2 upward, host byte order.
inline constexpr std::uint32_t kVirtualLanFirstAddress = 0x0AFE0002U;
inline constexpr std::size_t kVirtualLanMaxFriends = 16;

// A game that speaks UDP on a LAN, shown each connected friend as one machine on it. A datagram
// the game sends to a friend's address, on any port, goes sealed over that friend's connection;
// one from the friend arrives at the game's port on 127.0.0.1 from a loopback socket kept per
// friend and source port, which `source_of` names back as the friend's address and port. Every
// friend's datagrams are opened under its own keys and taken only from its peer. The LAN owns each
// connection's link purpose and sends a keepalive on it every second, which completes the host's
// meeting and holds NAT mappings open.
class VirtualLan {
public:
  VirtualLan() = default;
  ~VirtualLan();
  VirtualLan(const VirtualLan &) = delete;
  VirtualLan &operator=(const VirtualLan &) = delete;
  VirtualLan(VirtualLan &&) = delete;
  VirtualLan &operator=(VirtualLan &&) = delete;

  // The friend's address: `address` when the application numbers the session (so every machine
  // sees one friend at one address), else the next free one. Nothing when the LAN is full or the
  // address is taken.
  std::optional<std::uint32_t> add(std::unique_ptr<Connection> connection,
                                   std::optional<std::uint32_t> address = std::nullopt);
  bool is_friend(std::uint32_t address) const;
  std::vector<std::uint32_t> friends() const;

  // `bytes` from the game's `from_port` to `to`; false when `to` is no friend or its path refused.
  bool send(const net::Endpoint &to, std::uint16_t from_port, std::string_view bytes);
  // To `to_port` on every friend; how many took it.
  std::size_t broadcast(std::uint16_t to_port, std::uint16_t from_port, std::string_view bytes);
  // The friend's address and port a datagram from 127.0.0.1:`loopback_port` stands for.
  std::optional<net::Endpoint> source_of(std::uint16_t loopback_port) const;

private:
  struct Friend {
    std::uint32_t address = 0;
    std::unique_ptr<Connection> connection;
    std::unique_ptr<PacketSeal> seal;
    std::mutex send_mutex;
    Worker reader;
  };
  struct Proxy {
    net::Endpoint source;
    net::UdpSocket socket;
  };

  void read(const StopToken &stop, Friend &peer, std::unique_ptr<PacketOpener> opener);
  void deliver(const net::Endpoint &from, std::uint16_t to_port, std::string_view payload);
  Friend *find(std::uint32_t address) const;

  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<Friend>> friends_;
  // Loopback port to the friend endpoint it stands for, and back.
  std::map<std::uint16_t, Proxy> proxies_;
  std::map<std::pair<std::uint32_t, std::uint16_t>, std::uint16_t> proxy_of_;
};

} // namespace lucent::p2p

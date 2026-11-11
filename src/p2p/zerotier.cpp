#include "lucent/p2p/zerotier.h"

#include <ZeroTierSockets.h>

#include <array>
#include <cstring>

#include "lucent/log.h"

namespace lucent::p2p {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr auto kPollEvery = 50ms;

std::uint64_t adhoc_network(std::uint16_t port) {
  return zts_net_compute_adhoc_id(port, port);
}

// libzt's addresses hold the port in network order, whatever the host's.
std::uint16_t network_order(std::uint16_t port) {
  const std::array<std::uint8_t, 2> bytes{static_cast<std::uint8_t>(port >> 8u),
                                          static_cast<std::uint8_t>(port & 0xFFu)};
  std::uint16_t out = 0;
  std::memcpy(&out, bytes.data(), bytes.size());
  return out;
}

std::uint16_t host_order(std::uint16_t raw) {
  std::array<std::uint8_t, 2> bytes{};
  std::memcpy(bytes.data(), &raw, bytes.size());
  return static_cast<std::uint16_t>((bytes[0] << 8u) | bytes[1]);
}

zts_sockaddr_in6 socket_address(const PathAddress &address) {
  zts_sockaddr_in6 out{};
  out.sin6_len = sizeof out;
  out.sin6_family = ZTS_AF_INET6;
  out.sin6_port = network_order(address.port);
  std::memcpy(&out.sin6_addr, address.ip.data(), address.ip.size());
  return out;
}

// A bound socket on one ad-hoc network.
struct Bound {
  int socket = -1;
  std::uint16_t port = 0;
};

class ZeroTierPath final : public Path {
public:
  explicit ZeroTierPath(Bound bound) noexcept : socket_{bound.socket}, port_{bound.port} {
  }
  ~ZeroTierPath() override {
    zts_bsd_close(socket_);
  }
  ZeroTierPath(const ZeroTierPath &) = delete;
  ZeroTierPath &operator=(const ZeroTierPath &) = delete;
  ZeroTierPath(ZeroTierPath &&) = delete;
  ZeroTierPath &operator=(ZeroTierPath &&) = delete;

  Backend backend() const noexcept override {
    return Backend::ZeroTier;
  }
  std::uint16_t port() const noexcept override {
    return port_;
  }
  bool send(const PathAddress &to, std::string_view bytes) override {
    if (!to.v6) {
      return false;
    }
    const zts_sockaddr_in6 address = socket_address(to);
    return zts_bsd_sendto(socket_, bytes.data(), bytes.size(), 0,
                          reinterpret_cast<const zts_sockaddr *>(&address),
                          sizeof address) == static_cast<ssize_t>(bytes.size());
  }
  std::optional<std::size_t> receive(std::span<char> buffer, PathAddress &from,
                                     std::chrono::milliseconds wait) override {
    zts_pollfd ready{};
    ready.fd = socket_;
    ready.events = ZTS_POLLIN;
    if (zts_bsd_poll(&ready, 1, static_cast<int>(wait.count())) <= 0) {
      return std::nullopt;
    }
    zts_sockaddr_in6 sender{};
    zts_socklen_t size = sizeof sender;
    const ssize_t length = zts_bsd_recvfrom(socket_, buffer.data(), buffer.size(), ZTS_MSG_DONTWAIT,
                                            reinterpret_cast<zts_sockaddr *>(&sender), &size);
    if (length < 0 || sender.sin6_family != ZTS_AF_INET6) {
      return std::nullopt;
    }
    from.v6 = true;
    std::memcpy(from.ip.data(), &sender.sin6_addr, from.ip.size());
    from.port = host_order(sender.sin6_port);
    return static_cast<std::size_t>(length);
  }

private:
  int socket_;
  std::uint16_t port_;
};

} // namespace

std::unique_ptr<ZeroTierNode> ZeroTierNode::start(const ZeroTierConfig &config,
                                                  std::string &error) {
  std::error_code made;
  std::filesystem::create_directories(config.storage, made);
  if (made) {
    error = "no ZeroTier storage at " + config.storage.string() + ": " + made.message();
    return nullptr;
  }
  if (zts_init_from_storage(config.storage.string().c_str()) != ZTS_ERR_OK) {
    error = "ZeroTier cannot use its storage, or a node already runs";
    return nullptr;
  }
  if (!config.roots.empty() &&
      zts_init_set_roots(config.roots.data(), static_cast<unsigned int>(config.roots.size())) !=
          ZTS_ERR_OK) {
    error = "ZeroTier refused the root set";
    return nullptr;
  }
  // An ad-hoc network's configuration follows from its id; there is nothing to cache.
  if (zts_init_allow_net_cache(0) != ZTS_ERR_OK) {
    error = "ZeroTier refused its settings";
    return nullptr;
  }
  if (config.port != 0 && zts_init_set_port(config.port) != ZTS_ERR_OK) {
    error = "ZeroTier refused its port";
    return nullptr;
  }
  if (zts_node_start() != ZTS_ERR_OK) {
    error = "the ZeroTier node did not start";
    return nullptr;
  }
  lucent::log(Level::Info, "p2p", "the ZeroTier node is starting");
  return std::unique_ptr<ZeroTierNode>{new ZeroTierNode{config.network_port}};
}

ZeroTierNode::ZeroTierNode(std::uint16_t network_port)
    : network_port_{network_port}, joiner_{[this](const std::stop_token &stop) {
        join_when_online(stop);
      }} {
}

void ZeroTierNode::join_when_online(const std::stop_token &stop) const {
  while (!stop.stop_requested()) {
    if (zts_node_is_online() == 1) {
      if (zts_net_join(adhoc_network(network_port_)) != ZTS_ERR_OK) {
        lucent::log(Level::Warn, "p2p", "ZeroTier did not join the ad-hoc network");
      }
      return;
    }
    zts_util_delay(static_cast<unsigned long>(kPollEvery.count()));
  }
}

ZeroTierNode::~ZeroTierNode() {
  joiner_.request_stop();
  joiner_.join();
  zts_net_leave(adhoc_network(network_port_));
  zts_node_stop();
}

bool ZeroTierNode::wait_ready(const std::stop_token &stop, std::chrono::milliseconds wait) const {
  const auto until = Clock::now() + wait;
  while (!reach()) {
    if (stop.stop_requested() || Clock::now() >= until) {
      return false;
    }
    zts_util_delay(static_cast<unsigned long>(kPollEvery.count()));
  }
  return true;
}

std::optional<ZeroTierReach> ZeroTierNode::reach() const {
  const std::uint64_t network = adhoc_network(network_port_);
  if (zts_node_is_online() != 1 || zts_net_transport_is_ready(network) != 1 ||
      zts_addr_is_assigned(network, ZTS_AF_INET6) != 1) {
    return std::nullopt;
  }
  return ZeroTierReach{zts_node_get_id(), network_port_};
}

std::optional<PathAddress> ZeroTierNode::address_of(const ZeroTierReach &peer) const {
  if (peer.port != network_port_) {
    return std::nullopt;
  }
  zts_sockaddr_storage storage{};
  zts_addr_compute_6plane(adhoc_network(peer.port), peer.node, &storage);
  zts_sockaddr_in6 address{};
  std::memcpy(&address, &storage, sizeof address);
  PathAddress out;
  out.v6 = true;
  std::memcpy(out.ip.data(), &address.sin6_addr, out.ip.size());
  out.port = peer.port;
  return out;
}

std::unique_ptr<Path> ZeroTierNode::open(std::string &error) const {
  const int socket = zts_bsd_socket(ZTS_AF_INET6, ZTS_SOCK_DGRAM, 0);
  if (socket < 0) {
    error = "no ZeroTier socket";
    return nullptr;
  }
  zts_sockaddr_in6 local{};
  local.sin6_len = sizeof local;
  local.sin6_family = ZTS_AF_INET6;
  local.sin6_port = network_order(network_port_);
  if (zts_bsd_bind(socket, reinterpret_cast<const zts_sockaddr *>(&local), sizeof local) !=
      ZTS_ERR_OK) {
    zts_bsd_close(socket);
    error = "the ZeroTier port is taken";
    return nullptr;
  }
  return std::make_unique<ZeroTierPath>(Bound{socket, network_port_});
}

} // namespace lucent::p2p

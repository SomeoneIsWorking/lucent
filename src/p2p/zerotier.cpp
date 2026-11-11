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
    zts_net_leave(adhoc_network(port_));
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

PathAddress zerotier_address(std::uint64_t node, std::uint16_t port) {
  zts_sockaddr_storage storage{};
  zts_addr_compute_6plane(adhoc_network(port), node, &storage);
  zts_sockaddr_in6 address{};
  std::memcpy(&address, &storage, sizeof address);
  PathAddress out;
  out.v6 = true;
  std::memcpy(out.ip.data(), &address.sin6_addr, out.ip.size());
  out.port = port;
  return out;
}

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
  if (config.port != 0 && zts_init_set_port(config.port) != ZTS_ERR_OK) {
    error = "ZeroTier refused its port";
    return nullptr;
  }
  if (zts_node_start() != ZTS_ERR_OK) {
    error = "the ZeroTier node did not start";
    return nullptr;
  }
  lucent::log(Level::Info, "p2p", "the ZeroTier node is starting");
  return std::unique_ptr<ZeroTierNode>{new ZeroTierNode};
}

ZeroTierNode::~ZeroTierNode() {
  zts_node_stop();
}

bool ZeroTierNode::online() const noexcept {
  return zts_node_is_online() == 1;
}

bool ZeroTierNode::wait_online(const std::stop_token &stop, std::chrono::milliseconds wait) const {
  const auto until = Clock::now() + wait;
  while (!online()) {
    if (stop.stop_requested() || Clock::now() >= until) {
      return false;
    }
    zts_util_delay(static_cast<unsigned long>(kPollEvery.count()));
  }
  return true;
}

std::uint64_t ZeroTierNode::id() const noexcept {
  return zts_node_get_id();
}

std::unique_ptr<Path> ZeroTierNode::open(std::uint16_t port, std::string &error) const {
  const std::uint64_t network = adhoc_network(port);
  if (zts_net_join(network) != ZTS_ERR_OK) {
    error = "ZeroTier did not join the ad-hoc network";
    return nullptr;
  }
  const int socket = zts_bsd_socket(ZTS_AF_INET6, ZTS_SOCK_DGRAM, 0);
  if (socket < 0) {
    zts_net_leave(network);
    error = "no ZeroTier socket";
    return nullptr;
  }
  zts_sockaddr_in6 local{};
  local.sin6_len = sizeof local;
  local.sin6_family = ZTS_AF_INET6;
  local.sin6_port = network_order(port);
  if (zts_bsd_bind(socket, reinterpret_cast<const zts_sockaddr *>(&local), sizeof local) !=
      ZTS_ERR_OK) {
    zts_bsd_close(socket);
    zts_net_leave(network);
    error = "the ZeroTier port is taken";
    return nullptr;
  }
  return std::make_unique<ZeroTierPath>(Bound{socket, port});
}

} // namespace lucent::p2p

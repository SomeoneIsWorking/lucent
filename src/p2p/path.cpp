#include "lucent/p2p/path.h"

namespace lucent::p2p {

std::string_view name_of(Backend backend) noexcept {
  switch (backend) {
  case Backend::Direct:
    return "direct";
  case Backend::ZeroTier:
    return "zerotier";
  }
  return "unknown";
}

PathAddress PathAddress::from(const net::Endpoint &endpoint) noexcept {
  PathAddress address;
  address.ip[0] = static_cast<std::uint8_t>(endpoint.address >> 24u);
  address.ip[1] = static_cast<std::uint8_t>(endpoint.address >> 16u);
  address.ip[2] = static_cast<std::uint8_t>(endpoint.address >> 8u);
  address.ip[3] = static_cast<std::uint8_t>(endpoint.address);
  address.port = endpoint.port;
  return address;
}

std::optional<net::Endpoint> PathAddress::endpoint() const noexcept {
  if (v6) {
    return std::nullopt;
  }
  return net::Endpoint{(std::uint32_t{ip[0]} << 24u) | (std::uint32_t{ip[1]} << 16u) |
                           (std::uint32_t{ip[2]} << 8u) | ip[3],
                       port};
}

bool UdpPath::send(const PathAddress &to, std::string_view bytes) {
  const std::optional<net::Endpoint> endpoint = to.endpoint();
  return endpoint && socket_.send(*endpoint, bytes);
}

bool UdpPath::send_within(const PathAddress &to, std::string_view bytes, int hops) {
  const std::optional<net::Endpoint> endpoint = to.endpoint();
  return endpoint && socket_.send_within(*endpoint, bytes, hops);
}

std::optional<std::size_t> UdpPath::receive(std::span<char> buffer, PathAddress &from,
                                            std::chrono::milliseconds wait) {
  net::Endpoint sender;
  const std::optional<std::size_t> length = socket_.receive(buffer, sender, wait);
  if (length) {
    from = PathAddress::from(sender);
  }
  return length;
}

} // namespace lucent::p2p

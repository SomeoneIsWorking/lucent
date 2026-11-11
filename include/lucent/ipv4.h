#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lucent::net {

// An IPv4 address and port, both in host byte order like `Ipv4Interface`.
struct Endpoint {
  std::uint32_t address = 0;
  std::uint16_t port = 0;

  bool operator==(const Endpoint &) const = default;
};

// "192.168.1.20"; nothing when `text` is not a dotted IPv4 address.
std::optional<std::uint32_t> parse_ipv4(std::string_view text);
std::string format_ipv4(std::uint32_t address);
// "host:port" with a name or dotted address resolved to its first IPv4 address; nothing when it
// does not resolve or has no port.
std::optional<Endpoint> resolve_endpoint(std::string_view host_and_port);

// RFC 1918, shared (CGNAT and Tailscale), link-local or loopback: reachable only from inside a
// network.
bool private_ipv4(std::uint32_t address) noexcept;

} // namespace lucent::net

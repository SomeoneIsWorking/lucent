#include "lucent/ipv4.h"

#include <charconv>
#include <memory>

#include "http_socket.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#endif

namespace lucent::net {
namespace {

struct FreeAddresses {
  void operator()(addrinfo *list) const noexcept {
    freeaddrinfo(list);
  }
};

} // namespace

std::optional<std::uint32_t> parse_ipv4(std::string_view text) {
  std::uint32_t address = 0;
  const char *at = text.data();
  const char *end = text.data() + text.size();
  for (int part = 0; part < 4; ++part) {
    if (part > 0) {
      if (at == end || *at != '.') {
        return std::nullopt;
      }
      ++at;
    }
    unsigned value = 0;
    const auto [next, error] = std::from_chars(at, end, value);
    if (error != std::errc{} || value > 255u || next - at > 3) {
      return std::nullopt;
    }
    at = next;
    address = (address << 8u) | value;
  }
  if (at != end) {
    return std::nullopt;
  }
  return address;
}

std::string format_ipv4(std::uint32_t address) {
  return std::to_string(address >> 24u) + '.' + std::to_string((address >> 16u) & 0xFFu) + '.' +
         std::to_string((address >> 8u) & 0xFFu) + '.' + std::to_string(address & 0xFFu);
}

std::optional<Endpoint> resolve_endpoint(std::string_view host_and_port) {
  const std::size_t colon = host_and_port.rfind(':');
  if (colon == std::string_view::npos || colon == 0) {
    return std::nullopt;
  }
  std::uint16_t port = 0;
  const std::string_view port_text = host_and_port.substr(colon + 1);
  const auto [next, error] =
      std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
  if (error != std::errc{} || next != port_text.data() + port_text.size() || port == 0) {
    return std::nullopt;
  }
  const std::string host{host_and_port.substr(0, colon)};
  if (const std::optional<std::uint32_t> dotted = parse_ipv4(host)) {
    return Endpoint{*dotted, port};
  }
  if (!http::detail::initialize_socket_runtime()) {
    return std::nullopt;
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo *found = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || found == nullptr) {
    return std::nullopt;
  }
  const std::unique_ptr<addrinfo, FreeAddresses> list{found};
  const auto *address = reinterpret_cast<const sockaddr_in *>(list->ai_addr);
  return Endpoint{ntohl(address->sin_addr.s_addr), port};
}

bool private_ipv4(std::uint32_t address) noexcept {
  const auto in = [address](std::uint32_t network, unsigned bits) {
    return (address >> (32u - bits)) == (network >> (32u - bits));
  };
  return in(0x0A000000u, 8) || in(0xAC100000u, 12) || in(0xC0A80000u, 16) || in(0x64400000u, 10) ||
         in(0xA9FE0000u, 16) || in(0x7F000000u, 8);
}

} // namespace lucent::net

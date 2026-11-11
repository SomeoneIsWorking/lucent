#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "lucent/ipv4.h"

namespace lucent::net {

// The largest datagram a socket receives.
inline constexpr std::size_t kLargestDatagram = 65536;

// One IPv4 UDP socket bound on every interface. Waits are bounded so a reading thread can stop.
class UdpSocket {
public:
  // Binds `port`, a free one when 0. Nothing, with `error`, when the system refuses.
  static std::optional<UdpSocket> bind(std::uint16_t port, std::string &error);

  ~UdpSocket();
  UdpSocket(UdpSocket &&other) noexcept;
  UdpSocket &operator=(UdpSocket &&other) noexcept;
  UdpSocket(const UdpSocket &) = delete;
  UdpSocket &operator=(const UdpSocket &) = delete;

  std::uint16_t port() const noexcept {
    return port_;
  }
  // False when the system refused the datagram; a full buffer drops it.
  bool send(const Endpoint &to, std::string_view bytes) const noexcept;
  // A send that dies after `hops` routers: it opens this side's NAT and never reaches the far one.
  bool send_within(const Endpoint &to, std::string_view bytes, int hops) const noexcept;
  // The next datagram into `buffer`, waiting at most `wait`: its length, with its sender in `from`.
  std::optional<std::size_t> receive(std::span<char> buffer, Endpoint &from,
                                     std::chrono::milliseconds wait) const noexcept;

private:
  explicit UdpSocket(std::uintptr_t socket) noexcept : socket_(socket) {
  }
  void close() noexcept;

  std::uintptr_t socket_;
  std::uint16_t port_ = 0;
};

} // namespace lucent::net

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "lucent/udp_socket.h"

// A path is one backend's unauthenticated datagram socket: what the handshake, the reliable link
// and a game's session send over, whichever backend carries them. Nothing it receives is trusted
// until it opens under a key.
namespace lucent::p2p {

// The backends a descriptor can name, in the order a connector tries them.
enum class Backend : std::uint8_t {
  Direct = 0,
  ZeroTier = 1,
};

std::string_view name_of(Backend backend) noexcept;

// A peer's address on a path: IPv4 on the direct backend, IPv6 on ZeroTier.
struct PathAddress {
  bool v6 = false;
  // IPv4 in the first four bytes, in network order; IPv6 in all sixteen.
  std::array<std::uint8_t, 16> ip{};
  std::uint16_t port = 0;

  static PathAddress from(const net::Endpoint &endpoint) noexcept;
  // The IPv4 endpoint; nothing for an IPv6 address.
  std::optional<net::Endpoint> endpoint() const noexcept;
  bool operator==(const PathAddress &) const = default;
};

class Path {
public:
  Path() = default;
  Path(const Path &) = delete;
  Path &operator=(const Path &) = delete;
  Path(Path &&) = delete;
  Path &operator=(Path &&) = delete;
  virtual ~Path() = default;

  virtual Backend backend() const noexcept = 0;
  // The local port, which a router mapping or a ZeroTier peer address names.
  virtual std::uint16_t port() const noexcept = 0;
  virtual bool send(const PathAddress &to, std::string_view bytes) = 0;
  // A send that stops after `hops` routers; a backend without hops sends it in full.
  virtual bool send_within(const PathAddress &to, std::string_view bytes, int hops) {
    static_cast<void>(hops);
    return send(to, bytes);
  }
  // The next datagram, waiting at most `wait`: its length, with its sender in `from`.
  virtual std::optional<std::size_t> receive(std::span<char> buffer, PathAddress &from,
                                             std::chrono::milliseconds wait) = 0;
};

// The direct backend's path: one UDP socket.
class UdpPath final : public Path {
public:
  explicit UdpPath(net::UdpSocket socket) noexcept : socket_(std::move(socket)) {
  }

  Backend backend() const noexcept override {
    return Backend::Direct;
  }
  std::uint16_t port() const noexcept override {
    return socket_.port();
  }
  bool send(const PathAddress &to, std::string_view bytes) override;
  bool send_within(const PathAddress &to, std::string_view bytes, int hops) override;
  std::optional<std::size_t> receive(std::span<char> buffer, PathAddress &from,
                                     std::chrono::milliseconds wait) override;
  const net::UdpSocket &socket() const noexcept {
    return socket_;
  }

private:
  net::UdpSocket socket_;
};

} // namespace lucent::p2p

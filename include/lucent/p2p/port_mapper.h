#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

#include "lucent/udp_socket.h"

// Asking the router to forward a UDP port here, by whichever protocol it speaks (`NatPmp`,
// `UpnpIgd`). Always with a lease: a router offering only permanent mappings gets nothing, as a
// machine that dies would leave its port open.
namespace lucent::p2p {

// The most lease a mapping may have; a router's word for more is refused.
inline constexpr std::chrono::seconds kLongestLease{600};

struct Mapping {
  // Where the internet reaches it.
  net::Endpoint external;
  std::chrono::seconds lease{0};
};

class PortMapper {
public:
  PortMapper() = default;
  PortMapper(const PortMapper &) = delete;
  PortMapper &operator=(const PortMapper &) = delete;
  PortMapper(PortMapper &&) = delete;
  PortMapper &operator=(PortMapper &&) = delete;
  virtual ~PortMapper() = default;

  // Forwards a public port to local `port` for `lease`, or renews it; nothing when the router
  // will not or names an address that is not on the internet.
  virtual std::optional<Mapping> open(std::uint16_t port, std::chrono::seconds lease) = 0;
  // Takes the mapping of local `port` down.
  virtual void close(std::uint16_t port) = 0;
};

} // namespace lucent::p2p

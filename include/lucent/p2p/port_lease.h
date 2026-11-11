#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

#include "lucent/p2p/port_mapper.h"

// A router's mapping held while this object lives: renewed at half its lease on its own thread,
// taken down when it goes. Should the process die instead, the mapping lapses with its short lease.
namespace lucent::p2p {

// What a mapping is asked for, and renewed at half of.
inline constexpr std::chrono::seconds kAskedLease{120};

class PortLease {
public:
  // Keeps `mapping`, which `mapper` made for local `port`.
  PortLease(std::unique_ptr<PortMapper> mapper, std::uint16_t port, Mapping mapping);
  ~PortLease();
  PortLease(const PortLease &) = delete;
  PortLease &operator=(const PortLease &) = delete;
  PortLease(PortLease &&) = delete;
  PortLease &operator=(PortLease &&) = delete;

  const net::Endpoint &external() const noexcept {
    return external_;
  }

private:
  void keep(const std::stop_token &stop, std::chrono::seconds lease);

  std::unique_ptr<PortMapper> mapper_;
  std::uint16_t port_;
  net::Endpoint external_;
  std::jthread keeper_;
};

} // namespace lucent::p2p

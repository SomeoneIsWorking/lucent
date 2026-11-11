#pragma once

#include <cassert>
#include <memory>
#include <string>

#include "lucent/p2p/path.h"

namespace lucent::p2p::test {

inline std::unique_ptr<UdpPath> bound_path() {
  std::string error;
  std::optional<net::UdpSocket> socket = net::UdpSocket::bind(0, error);
  assert(socket && error.empty());
  return std::make_unique<UdpPath>(std::move(*socket));
}

inline PathAddress loopback_of(const Path &path) {
  return PathAddress::from(net::Endpoint{0x7F000001u, path.port()});
}

} // namespace lucent::p2p::test

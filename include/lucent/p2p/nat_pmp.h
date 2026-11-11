#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "lucent/p2p/port_mapper.h"

// A port mapping by NAT-PMP (RFC 6886): one small datagram to the gateway and its answer.
namespace lucent::p2p {

inline constexpr std::uint16_t kNatPmpPort = 5351;

class NatPmp final : public PortMapper {
public:
  // Asks `gateway`, and takes answers from it only. Nothing, with `error`, without a socket.
  static std::unique_ptr<NatPmp> create(net::Endpoint gateway, std::string &error);

  std::optional<Mapping> open(std::uint16_t port, std::chrono::seconds lease) override;
  void close(std::uint16_t port) override;

private:
  NatPmp(net::Endpoint gateway, net::UdpSocket socket);
  // The gateway's answer to `request`, at least `answer_bytes` long with a good result code.
  std::optional<std::string> ask(std::string_view request, std::size_t answer_bytes);

  net::Endpoint gateway_;
  net::UdpSocket socket_;
  std::uint16_t external_ = 0;
};

} // namespace lucent::p2p

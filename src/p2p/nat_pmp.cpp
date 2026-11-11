#include "lucent/p2p/nat_pmp.h"

#include <array>

#include "wire_bytes.h"

namespace lucent::p2p {
namespace {

using namespace std::chrono_literals;

constexpr std::uint8_t kOpExternalAddress = 0;
constexpr std::uint8_t kOpMapUdp = 1;
constexpr std::uint8_t kAnswerBit = 128;
constexpr std::size_t kAddressAnswerBytes = 12;
constexpr std::size_t kMapAnswerBytes = 16;
constexpr std::array kWaits{250ms, 500ms, 1000ms};

std::string map_request(std::uint16_t port, std::uint16_t external, std::chrono::seconds lease) {
  std::string request;
  request.push_back(0);
  request.push_back(static_cast<char>(kOpMapUdp));
  wire::append_u16(request, 0);
  wire::append_u16(request, port);
  wire::append_u16(request, external);
  wire::append_u32(request, static_cast<std::uint32_t>(lease.count()));
  return request;
}

} // namespace

std::unique_ptr<NatPmp> NatPmp::create(net::Endpoint gateway, std::string &error) {
  std::optional<net::UdpSocket> socket = net::UdpSocket::bind(0, error);
  if (!socket) {
    return nullptr;
  }
  return std::unique_ptr<NatPmp>{new NatPmp{gateway, std::move(*socket)}};
}

NatPmp::NatPmp(net::Endpoint gateway, net::UdpSocket socket)
    : gateway_{gateway}, socket_{std::move(socket)} {
}

std::optional<std::string> NatPmp::ask(std::string_view request, std::size_t answer_bytes) {
  std::array<char, 64> buffer{};
  const auto op = static_cast<std::uint8_t>(request[1]);
  for (const std::chrono::milliseconds wait : kWaits) {
    socket_.send(gateway_, request);
    const auto until = std::chrono::steady_clock::now() + wait;
    while (std::chrono::steady_clock::now() < until) {
      net::Endpoint from;
      const std::optional<std::size_t> length = socket_.receive(buffer, from, wait);
      if (!length || from != gateway_ || *length < answer_bytes) {
        continue;
      }
      const std::string_view answer{buffer.data(), *length};
      if (static_cast<std::uint8_t>(answer[1]) != (op | kAnswerBit)) {
        continue;
      }
      if (wire::u16_at(answer, 2) != 0) {
        return std::nullopt;
      }
      return std::string{answer};
    }
  }
  return std::nullopt;
}

std::optional<Mapping> NatPmp::open(std::uint16_t port, std::chrono::seconds lease) {
  const std::string address_request{static_cast<char>(0), static_cast<char>(kOpExternalAddress)};
  const std::optional<std::string> address = ask(address_request, kAddressAnswerBytes);
  if (!address) {
    return std::nullopt;
  }
  Mapping mapping;
  mapping.external.address = wire::u32_at(*address, 8);
  if (net::private_ipv4(mapping.external.address)) {
    return std::nullopt;
  }
  const std::uint16_t wanted = external_ != 0 ? external_ : port;
  const std::optional<std::string> mapped = ask(map_request(port, wanted, lease), kMapAnswerBytes);
  if (!mapped || wire::u16_at(*mapped, 8) != port) {
    return std::nullopt;
  }
  external_ = wire::u16_at(*mapped, 10);
  mapping.external.port = external_;
  mapping.lease = std::chrono::seconds{wire::u32_at(*mapped, 12)};
  if (mapping.lease == 0s || mapping.lease > kLongestLease) {
    close(port);
    return std::nullopt;
  }
  return mapping;
}

void NatPmp::close(std::uint16_t port) {
  static_cast<void>(ask(map_request(port, 0, 0s), kMapAnswerBytes));
  external_ = 0;
}

} // namespace lucent::p2p

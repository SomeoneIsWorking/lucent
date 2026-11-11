#include "lucent/p2p/path_bridge.h"

#include <utility>
#include <vector>

namespace lucent::p2p {
namespace {

using namespace std::chrono_literals;

constexpr auto kWait = 100ms;
constexpr std::uint32_t kLoopback = 0x7F000001u;
constexpr std::uint32_t kLoopbackMask = 0xFF000000u;

bool on_loopback(const net::Endpoint &endpoint) noexcept {
  return (endpoint.address & kLoopbackMask) == (kLoopback & kLoopbackMask);
}

} // namespace

std::unique_ptr<PathBridge> PathBridge::start(std::unique_ptr<Path> path, PathAddress peer,
                                              std::string &error) {
  std::optional<net::UdpSocket> socket = net::UdpSocket::bind(0, error);
  if (!socket) {
    return nullptr;
  }
  return std::unique_ptr<PathBridge>{new PathBridge{std::move(path), peer, std::move(*socket)}};
}

PathBridge::PathBridge(std::unique_ptr<Path> path, PathAddress peer, net::UdpSocket socket)
    : path_{std::move(path)}, peer_{peer}, socket_{std::move(socket)},
      outward_{[this](const StopToken &stop) {
        outward(stop);
      }},
      inward_{[this](const StopToken &stop) {
        inward(stop);
      }} {
}

net::Endpoint PathBridge::local() const noexcept {
  return {kLoopback, socket_.port()};
}

void PathBridge::outward(const StopToken &stop) {
  std::vector<char> buffer(net::kLargestDatagram);
  while (!stop.stop_requested()) {
    net::Endpoint from;
    const std::optional<std::size_t> length = socket_.receive(buffer, from, kWait);
    if (!length || !on_loopback(from)) {
      continue;
    }
    {
      const std::lock_guard lock{mutex_};
      process_ = from;
    }
    path_->send(peer_, {buffer.data(), *length});
  }
}

void PathBridge::inward(const StopToken &stop) {
  std::vector<char> buffer(net::kLargestDatagram);
  while (!stop.stop_requested()) {
    PathAddress from;
    const std::optional<std::size_t> length = path_->receive(buffer, from, kWait);
    if (!length || from != peer_) {
      continue;
    }
    std::optional<net::Endpoint> process;
    {
      const std::lock_guard lock{mutex_};
      process = process_;
    }
    if (process) {
      socket_.send(*process, {buffer.data(), *length});
    }
  }
}

} // namespace lucent::p2p

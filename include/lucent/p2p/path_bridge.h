#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

#include "lucent/p2p/path.h"
#include "lucent/udp_socket.h"

// Carries a local process's datagrams over a path it cannot open itself: an embedded ZeroTier
// node lives in one process, and a game runner is another. The process sends to `local()`; the
// bridge sends that to the peer over the path and hands back what the peer sends. It reads nothing:
// the process seals its own datagrams.
namespace lucent::p2p {

class PathBridge {
public:
  // A bridge to `peer` over `path`, or nothing with the reason in `error`.
  static std::unique_ptr<PathBridge> start(std::unique_ptr<Path> path, PathAddress peer,
                                           std::string &error);
  ~PathBridge() = default;
  PathBridge(const PathBridge &) = delete;
  PathBridge &operator=(const PathBridge &) = delete;
  PathBridge(PathBridge &&) = delete;
  PathBridge &operator=(PathBridge &&) = delete;

  // Where the local process sends, on this machine's loopback.
  net::Endpoint local() const noexcept;

private:
  PathBridge(std::unique_ptr<Path> path, PathAddress peer, net::UdpSocket socket);
  void outward(const std::stop_token &stop);
  void inward(const std::stop_token &stop);

  std::unique_ptr<Path> path_;
  PathAddress peer_;
  net::UdpSocket socket_;
  mutable std::mutex mutex_;
  // The local process, once it has sent; only loopback senders are taken.
  std::optional<net::Endpoint> process_;
  std::jthread outward_;
  std::jthread inward_;
};

} // namespace lucent::p2p

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>

#include "lucent/p2p/zerotier_paths.h"

// The embedded ZeroTier backend (`lucent::zerotier`): one libzt node in the process on one
// controller-less ad-hoc network, the application's. No install, root or account: the node is a
// library. Nothing it carries is trusted until it opens under the code's key, as on any path.
namespace lucent::p2p {

struct ZeroTierConfig {
  // Where the node keeps its identity, so its address stays the same between runs.
  std::filesystem::path storage;
  // A signed root set (`zts_util_sign_root_set`); empty uses ZeroTier's public roots.
  std::string roots;
  // The node's own UDP port on the real network; 0 lets it choose.
  std::uint16_t port = 0;
  // The application's port, which names its ad-hoc network and the UDP port on it.
  std::uint16_t network_port = 0;
};

// A root of one's own, for a lab with no internet: a fresh identity in `storage`, and the signed
// root set naming it at `endpoint` ("address/port"), which every node there is given as
// `ZeroTierConfig::roots`. Nothing, with the reason in `error`, if it cannot be made.
std::optional<std::string> make_root(const std::filesystem::path &storage,
                                     const std::string &endpoint, std::string &error);

// The process's one ZeroTier node; libzt refuses a second. Started when the application's online
// menu opens, so it has its roots and its network before a code is made or pasted.
class ZeroTierNode final : public ZeroTierPaths {
public:
  // A started node joining its network, or nothing with the reason in `error`.
  static std::unique_ptr<ZeroTierNode> start(const ZeroTierConfig &config, std::string &error);
  ~ZeroTierNode() override;
  ZeroTierNode(const ZeroTierNode &) = delete;
  ZeroTierNode &operator=(const ZeroTierNode &) = delete;
  ZeroTierNode(ZeroTierNode &&) = delete;
  ZeroTierNode &operator=(ZeroTierNode &&) = delete;

  // Waits at most `wait` for the node to have its address on the network.
  bool wait_ready(const std::stop_token &stop, std::chrono::milliseconds wait) const;

  std::optional<ZeroTierReach> reach() const override;
  std::unique_ptr<Path> open(std::string &error) const override;
  std::optional<PathAddress> address_of(const ZeroTierReach &peer) const override;

private:
  explicit ZeroTierNode(std::uint16_t network_port);
  // libzt joins a network only once the node is online.
  void join_when_online(const std::stop_token &stop) const;

  std::uint16_t network_port_;
  std::jthread joiner_;
};

} // namespace lucent::p2p

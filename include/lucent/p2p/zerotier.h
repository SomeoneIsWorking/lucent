#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>

#include "lucent/p2p/path.h"

// The embedded ZeroTier backend: one libzt node in the process, and a path per port on the
// controller-less ad-hoc network that port names. No install, root or account: the node is a
// library. Nothing it carries is trusted until it opens under the code's key, as on any path.
namespace lucent::p2p {

struct ZeroTierConfig {
  // Where the node keeps its identity, so its address stays the same between runs.
  std::filesystem::path storage;
  // A signed root set (`zts_util_sign_root_set`); empty uses ZeroTier's public roots.
  std::string roots;
  // The node's UDP port; 0 lets it choose.
  std::uint16_t port = 0;
};

// A peer's address on the ad-hoc network for `port`: its 6PLANE address there.
PathAddress zerotier_address(std::uint64_t node, std::uint16_t port);

// The process's one ZeroTier node; libzt refuses a second.
class ZeroTierNode {
public:
  // A started node, or nothing with the reason in `error`. It comes online on its own.
  static std::unique_ptr<ZeroTierNode> start(const ZeroTierConfig &config, std::string &error);
  ~ZeroTierNode();
  ZeroTierNode(const ZeroTierNode &) = delete;
  ZeroTierNode &operator=(const ZeroTierNode &) = delete;
  ZeroTierNode(ZeroTierNode &&) = delete;
  ZeroTierNode &operator=(ZeroTierNode &&) = delete;

  bool online() const noexcept;
  // Waits at most `wait` for the node to reach its roots.
  bool wait_online(const std::stop_token &stop, std::chrono::milliseconds wait) const;
  // The node's 40-bit address.
  std::uint64_t id() const noexcept;
  // A path on the ad-hoc network for `port`, or nothing with the reason in `error`. It sends
  // nothing until the node has its address there, which takes a few seconds after joining.
  std::unique_ptr<Path> open(std::uint16_t port, std::string &error) const;

private:
  ZeroTierNode() = default;
};

} // namespace lucent::p2p

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "lucent/p2p/descriptor.h"
#include "lucent/p2p/path.h"

// The seam the connector reaches ZeroTier through. `lucent::zerotier` implements it with an
// embedded node; an application that links no ZeroTier gives the connector none.
namespace lucent::p2p {

class ZeroTierPaths {
public:
  ZeroTierPaths() = default;
  ZeroTierPaths(const ZeroTierPaths &) = delete;
  ZeroTierPaths &operator=(const ZeroTierPaths &) = delete;
  ZeroTierPaths(ZeroTierPaths &&) = delete;
  ZeroTierPaths &operator=(ZeroTierPaths &&) = delete;
  virtual ~ZeroTierPaths() = default;

  // This node on its network, as a code names it; nothing until it has its address there.
  virtual std::optional<ZeroTierReach> reach() const = 0;
  // A path on that network, or nothing with the reason in `error`.
  virtual std::unique_ptr<Path> open(std::string &error) const = 0;
  // Where `peer` is on that network; nothing when its code names another network.
  virtual std::optional<PathAddress> address_of(const ZeroTierReach &peer) const = 0;
};

} // namespace lucent::p2p

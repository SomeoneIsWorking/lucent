#include "lucent/p2p/port_lease.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include "lucent/log.h"
#include "lucent/worker.h"

namespace lucent::p2p {
namespace {

using namespace std::chrono_literals;

// How soon to try again after a renewal the router did not answer.
constexpr std::chrono::seconds kRetry{5};

} // namespace

PortLease::PortLease(std::unique_ptr<PortMapper> mapper, std::uint16_t port, Mapping mapping)
    : mapper_{std::move(mapper)}, port_{port}, external_{mapping.external},
      keeper_{[this, lease = mapping.lease](const StopToken &stop) {
        keep(stop, lease);
      }} {
  lucent::log(lucent::Level::Info, "p2p",
              "the router forwards a port here for " + std::to_string(mapping.lease.count()) +
                  " s at a time");
}

PortLease::~PortLease() {
  keeper_.request_stop();
  keeper_.join();
  mapper_->close(port_);
  lucent::log(lucent::Level::Info, "p2p", "the router's port mapping is taken down");
}

void PortLease::keep(const StopToken &stop, std::chrono::seconds lease) {
  std::chrono::seconds wait = std::max(lease / 2, std::chrono::seconds{1});
  while (!stop.sleep_for(wait)) {
    const std::optional<Mapping> renewed = mapper_->open(port_, kAskedLease);
    wait = renewed ? std::max(renewed->lease / 2, std::chrono::seconds{1}) : kRetry;
  }
}

} // namespace lucent::p2p

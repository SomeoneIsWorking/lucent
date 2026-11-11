#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

#include "lucent/p2p/port_mapper.h"

// A port mapping through a router's UPnP Internet Gateway Device: found by SSDP, its description
// read for the WAN connection's control URL, then AddPortMapping and DeletePortMapping by SOAP.
// What a device on the LAN says is not trusted: its URLs must be on the address that answered the
// search, bodies are capped, and nothing is followed elsewhere.
namespace lucent::p2p {

inline constexpr std::uint16_t kSsdpPort = 1900;

// The control path and service type of a description's WAN connection; nothing when it lists
// none or names another host than `origin` ("http://host:port").
struct IgdService {
  std::string control_path;
  std::string service_type;
};
std::optional<IgdService> igd_service_in(std::string_view description, std::string_view origin);

// The LOCATION header of an SSDP answer.
std::optional<std::string> ssdp_location(std::string_view answer);

class UpnpIgd final : public PortMapper {
public:
  // Searches `ssdp` for a router for at most `wait` and reads its description; nothing when none
  // answers. `local` is where its mappings point, in host byte order.
  static std::unique_ptr<UpnpIgd> discover(const std::stop_token &stop, net::Endpoint ssdp,
                                           std::uint32_t local, std::chrono::milliseconds wait);

  std::optional<Mapping> open(std::uint16_t port, std::chrono::seconds lease) override;
  void close(std::uint16_t port) override;

private:
  UpnpIgd(net::Endpoint router, IgdService service, std::uint32_t local);
  // The body of a successful SOAP `action`; a failure's UPnP error code goes in `error`.
  std::optional<std::string> call(std::string_view action, const std::string &arguments,
                                  int &error) const;

  net::Endpoint router_;
  IgdService service_;
  std::uint32_t local_;
  std::uint16_t external_ = 0;
};

} // namespace lucent::p2p

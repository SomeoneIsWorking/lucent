#include "lucent/p2p/nat_pmp.h"
#include "lucent/p2p/port_lease.h"
#include "lucent/p2p/upnp_igd.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "lucent/http.h"

namespace {

using namespace lucent::p2p;
using namespace std::chrono_literals;
using lucent::net::Endpoint;
using lucent::net::UdpSocket;

constexpr std::uint32_t kLoopback = 0x7F000001u;
// 203.0.113.9, the stand-in routers' public address.
constexpr std::uint32_t kPublic = 0xCB007109u;

UdpSocket bound() {
  std::string error;
  std::optional<UdpSocket> socket = UdpSocket::bind(0, error);
  assert(socket);
  return std::move(*socket);
}

std::uint16_t word_at(std::string_view bytes, std::size_t at) {
  return static_cast<std::uint16_t>((static_cast<std::uint8_t>(bytes[at]) << 8u) |
                                    static_cast<std::uint8_t>(bytes[at + 1]));
}

// A NAT-PMP gateway granting `granted` seconds at 203.0.113.9.
class Gateway {
public:
  explicit Gateway(std::uint32_t granted)
      : granted_{granted}, thread_{[this](const std::stop_token &stop) {
          serve(stop);
        }} {
  }

  Endpoint address() const {
    return {kLoopback, socket_.port()};
  }
  int maps() const {
    return maps_;
  }
  int removals() const {
    return removals_;
  }

private:
  void serve(const std::stop_token &stop) {
    std::vector<char> buffer(64);
    while (!stop.stop_requested()) {
      Endpoint from;
      const auto length = socket_.receive(buffer, from, 20ms);
      if (!length || *length < 2) {
        continue;
      }
      std::string answer(2, '\0');
      answer[1] = static_cast<char>(static_cast<std::uint8_t>(buffer[1]) | 0x80u);
      answer.append(6, '\0');
      if (buffer[1] == 0) {
        answer.append("\xCB\x00\x71\x09", 4);
      } else {
        const std::string_view request{buffer.data(), *length};
        const std::uint32_t asked =
            (std::uint32_t{word_at(request, 8)} << 16u) | word_at(request, 10);
        asked == 0 ? ++removals_ : ++maps_;
        answer.append(request.substr(4, 2));
        answer.append(request.substr(4, 2));
        const std::uint32_t lifetime = asked == 0 ? 0 : granted_;
        for (int shift = 24; shift >= 0; shift -= 8) {
          answer.push_back(static_cast<char>((lifetime >> shift) & 0xFFu));
        }
      }
      socket_.send(from, answer);
    }
  }

  std::uint32_t granted_;
  UdpSocket socket_{bound()};
  std::atomic<int> maps_{0};
  std::atomic<int> removals_{0};
  std::jthread thread_;
};

std::unique_ptr<NatPmp> nat_pmp(Endpoint gateway) {
  std::string error;
  std::unique_ptr<NatPmp> mapper = NatPmp::create(gateway, error);
  assert(mapper);
  return mapper;
}

void a_lease_is_kept_renewed_and_taken_down() {
  const Gateway gateway{2};
  {
    std::unique_ptr<NatPmp> mapper = nat_pmp(gateway.address());
    const std::optional<Mapping> mapping = mapper->open(4000, kAskedLease);
    assert(mapping && mapping->lease == 2s);
    assert(mapping->external == (Endpoint{kPublic, 4000}));
    const PortLease lease{std::move(mapper), 4000, *mapping};
    std::this_thread::sleep_for(2500ms);
    // Renewed at half its time, and up meanwhile.
    assert(gateway.maps() >= 3);
    assert(gateway.removals() == 0);
  }
  assert(gateway.removals() == 1);
}

void a_long_lease_is_refused_and_taken_down() {
  const Gateway gateway{86400};
  std::unique_ptr<NatPmp> mapper = nat_pmp(gateway.address());
  assert(!mapper->open(4000, kAskedLease));
  assert(gateway.removals() == 1);
}

void no_gateway_gives_up_soon() {
  const UdpSocket silent = bound();
  std::unique_ptr<NatPmp> mapper = nat_pmp({kLoopback, silent.port()});
  const auto started = std::chrono::steady_clock::now();
  assert(!mapper->open(4000, kAskedLease));
  assert(std::chrono::steady_clock::now() - started < 5s);
}

// How a stand-in UPnP router answers.
struct RouterSetup {
  // The UPnP error AddPortMapping answers with, 0 for none.
  int refuse_with = 0;
  std::string external_ip = "203.0.113.9";
  // The SSDP answer's LOCATION; the router's own description when not set.
  std::optional<std::string> location;
};

// A router answering SSDP on one socket and SOAP on a loopback server.
class Router {
public:
  explicit Router(RouterSetup setup)
      : setup_{std::move(setup)}, soap_{{}, [this](const lucent::http::Request &request) {
                                          return answer(request);
                                        }} {
    const bool started = soap_.start();
    assert(started);
    static_cast<void>(started);
    location_ =
        setup_.location.value_or("http://127.0.0.1:" + std::to_string(soap_.port()) + "/desc.xml");
    answering_ = std::jthread{[this](const std::stop_token &stop) {
      std::vector<char> buffer(2048);
      while (!stop.stop_requested()) {
        Endpoint from;
        if (ssdp_.receive(buffer, from, 20ms)) {
          ssdp_.send(from, "HTTP/1.1 200 OK\r\nLOCATION: " + location_ + "\r\n\r\n");
        }
      }
    }};
  }

  Endpoint ssdp() const {
    return {kLoopback, ssdp_.port()};
  }
  bool did(const std::string &action) {
    const std::lock_guard lock{mutex_};
    return std::find(actions_.begin(), actions_.end(), action) != actions_.end();
  }

private:
  lucent::http::Response answer(const lucent::http::Request &request) {
    using lucent::http::Response;
    if (request.method == "GET") {
      return Response::text(
          200, "OK",
          "<root><device><serviceList><service><serviceType>"
          "urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
          "<controlURL>/ctl</controlURL></service></serviceList></device></root>");
    }
    const std::lock_guard lock{mutex_};
    if (request.body.find("GetExternalIPAddress") != std::string::npos) {
      actions_.emplace_back("external");
      return Response::text(200, "OK",
                            "<u:R><NewExternalIPAddress>" + setup_.external_ip +
                                "</NewExternalIPAddress></u:R>");
    }
    if (request.body.find("DeletePortMapping") != std::string::npos) {
      actions_.emplace_back("delete");
      return Response::text(200, "OK", "<ok/>");
    }
    actions_.emplace_back(
        request.body.find("<NewLeaseDuration>120<") != std::string::npos ? "add 120" : "add other");
    if (setup_.refuse_with != 0) {
      return Response::text(500, "Internal Server Error",
                            "<UPnPError><errorCode>" + std::to_string(setup_.refuse_with) +
                                "</errorCode></UPnPError>");
    }
    return Response::text(200, "OK", "<ok/>");
  }

  RouterSetup setup_;
  std::mutex mutex_;
  std::vector<std::string> actions_;
  lucent::http::Server soap_;
  std::string location_;
  UdpSocket ssdp_{bound()};
  std::jthread answering_;
};

std::unique_ptr<UpnpIgd> found(const Router &router) {
  return UpnpIgd::discover(std::stop_token{}, router.ssdp(), kLoopback, 500ms);
}

void upnp_maps_with_a_short_lease_and_deletes() {
  Router router{{}};
  std::unique_ptr<UpnpIgd> igd = found(router);
  assert(igd);
  const std::optional<Mapping> mapping = igd->open(4000, kAskedLease);
  assert(mapping && mapping->external == (Endpoint{kPublic, 4000}));
  assert(router.did("add 120"));
  igd->close(4000);
  assert(router.did("delete"));
}

void upnp_offering_only_permanent_mappings_gets_none() {
  Router router{{.refuse_with = 725}};
  std::unique_ptr<UpnpIgd> igd = found(router);
  assert(igd);
  assert(!igd->open(4000, kAskedLease));
  // No mapping without a lease is asked for.
  assert(!router.did("add other"));
}

void upnp_behind_a_double_nat_maps_nothing() {
  Router router{{.external_ip = "192.168.1.9"}};
  std::unique_ptr<UpnpIgd> igd = found(router);
  assert(igd && !igd->open(4000, kAskedLease));
  assert(!router.did("add 120"));
}

void upnp_location_on_another_host_is_not_followed() {
  const Router router{{.location = "http://198.51.100.7:80/desc.xml"}};
  assert(!found(router));
}

void a_description_names_its_control_path() {
  const std::string origin = "http://192.168.1.1:5000";
  const auto service = igd_service_in("<serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1"
                                      "</serviceType><controlURL>ctl/IPConn</controlURL>",
                                      origin);
  assert(service && service->control_path == "/ctl/IPConn");
  assert(service->service_type == "urn:schemas-upnp-org:service:WANPPPConnection:1");
  assert(!igd_service_in("<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1"
                         "</serviceType><controlURL>http://203.0.113.1/ctl</controlURL>",
                         origin));
}

} // namespace

int main() {
  a_lease_is_kept_renewed_and_taken_down();
  a_long_lease_is_refused_and_taken_down();
  no_gateway_gives_up_soon();
  a_description_names_its_control_path();
  upnp_maps_with_a_short_lease_and_deletes();
  upnp_offering_only_permanent_mappings_gets_none();
  upnp_behind_a_double_nat_maps_nothing();
  upnp_location_on_another_host_is_not_followed();
  std::puts("lucent p2p port lease tests passed");
  return 0;
}

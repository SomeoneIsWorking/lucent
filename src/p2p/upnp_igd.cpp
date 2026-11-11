#include "lucent/p2p/upnp_igd.h"

#include <array>
#include <charconv>
#include <vector>

#include "lucent/http_client.h"
#include "lucent/p2p/crypto.h"
#include "lucent/text.h"
#include "lucent/udp_socket.h"

namespace lucent::p2p {
namespace {

using namespace std::chrono_literals;

constexpr std::array kSearchedTargets{
    std::string_view{"urn:schemas-upnp-org:device:InternetGatewayDevice:2"},
    std::string_view{"urn:schemas-upnp-org:device:InternetGatewayDevice:1"},
};
constexpr std::array kConnectionTypes{
    std::string_view{"urn:schemas-upnp-org:service:WANIPConnection:2"},
    std::string_view{"urn:schemas-upnp-org:service:WANIPConnection:1"},
    std::string_view{"urn:schemas-upnp-org:service:WANPPPConnection:1"},
};
constexpr std::size_t kMostBody = std::size_t{256} * 1024;
constexpr int kConflictInMapping = 718;
// How many other public ports to try when the one wanted is taken.
constexpr int kPortTries = 4;
constexpr std::uint16_t kFirstRandomPort = 20000;
constexpr std::uint16_t kRandomPorts = 40000;
constexpr std::string_view kScheme = "http://";

// The text inside the first `<tag>` of `xml`, a namespace prefix on it allowed.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::optional<std::string_view> element(std::string_view xml, std::string_view tag) {
  const std::string open = std::string{tag} + ">";
  for (std::size_t at = xml.find(open); at != std::string_view::npos; at = xml.find(open, at + 1)) {
    const std::size_t start = xml.rfind('<', at);
    if (start == std::string_view::npos || xml[start + 1] == '/') {
      continue;
    }
    const std::size_t text = at + open.size();
    const std::size_t end = xml.find('<', text);
    if (end == std::string_view::npos) {
      return std::nullopt;
    }
    return xml.substr(text, end - text);
  }
  return std::nullopt;
}

// "http://host:port" of a plain-HTTP `url`.
std::optional<std::string_view> origin_of(std::string_view url) {
  if (!url.starts_with(kScheme)) {
    return std::nullopt;
  }
  return url.substr(0, url.find('/', kScheme.size()));
}

// The router `origin` names, which must be a dotted address with a port (80 when none).
std::optional<net::Endpoint> endpoint_of(std::string_view origin) {
  const std::string_view host = origin.substr(kScheme.size());
  const std::size_t colon = host.find(':');
  const std::optional<std::uint32_t> address = net::parse_ipv4(host.substr(0, colon));
  if (!address) {
    return std::nullopt;
  }
  std::uint16_t port = 80;
  if (colon != std::string_view::npos) {
    const std::string_view text = host.substr(colon + 1);
    const auto [next, error] = std::from_chars(text.data(), text.data() + text.size(), port);
    if (error != std::errc{} || next != text.data() + text.size()) {
      return std::nullopt;
    }
  }
  return net::Endpoint{*address, port};
}

std::string search_for(std::string_view target) {
  return "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: "
         "1\r\nST: " +
         std::string{target} + "\r\n\r\n";
}

std::uint16_t random_port() {
  const crypto::Bytes random = crypto::random_bytes(2);
  return static_cast<std::uint16_t>(kFirstRandomPort +
                                    ((random[0] << 8u) | random[1]) % kRandomPorts);
}

} // namespace

std::optional<std::string> ssdp_location(std::string_view answer) {
  const std::string low = text::ascii_lower(answer);
  constexpr std::string_view kHeader = "\r\nlocation:";
  const std::size_t at = low.find(kHeader);
  if (at == std::string::npos) {
    return std::nullopt;
  }
  std::size_t start = at + kHeader.size();
  const std::size_t end = low.find("\r\n", start);
  if (end == std::string::npos) {
    return std::nullopt;
  }
  while (start < end && answer[start] == ' ') {
    ++start;
  }
  return std::string{answer.substr(start, end - start)};
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::optional<IgdService> igd_service_in(std::string_view description, std::string_view origin) {
  for (const std::string_view type : kConnectionTypes) {
    const std::size_t at = description.find(type);
    if (at == std::string_view::npos) {
      continue;
    }
    const std::optional<std::string_view> control = element(description.substr(at), "controlURL");
    if (!control || control->empty()) {
      continue;
    }
    if (control->front() == '/') {
      return IgdService{std::string{*control}, std::string{type}};
    }
    if (origin_of(*control) == origin) {
      return IgdService{std::string{control->substr(origin.size())}, std::string{type}};
    }
    if (control->find("://") == std::string_view::npos) {
      return IgdService{"/" + std::string{*control}, std::string{type}};
    }
  }
  return std::nullopt;
}

std::unique_ptr<UpnpIgd> UpnpIgd::discover(const StopToken &stop, net::Endpoint ssdp,
                                           std::uint32_t local, std::chrono::milliseconds wait) {
  std::string error;
  const std::optional<net::UdpSocket> socket = net::UdpSocket::bind(0, error);
  if (!socket) {
    return nullptr;
  }
  for (const std::string_view target : kSearchedTargets) {
    socket->send(ssdp, search_for(target));
  }
  std::vector<char> buffer(net::kLargestDatagram);
  const auto until = std::chrono::steady_clock::now() + wait;
  while (std::chrono::steady_clock::now() < until && !stop.stop_requested()) {
    net::Endpoint from;
    const std::optional<std::size_t> length = socket->receive(buffer, from, 100ms);
    if (!length) {
      continue;
    }
    const std::optional<std::string> location = ssdp_location({buffer.data(), *length});
    const std::optional<std::string_view> origin = location ? origin_of(*location) : std::nullopt;
    const std::optional<net::Endpoint> router = origin ? endpoint_of(*origin) : std::nullopt;
    if (!router || router->address != from.address) {
      continue;
    }
    http::FetchResult answer;
    const std::string path{std::string_view{*location}.substr(origin->size())};
    if (!http::fetch(
            *router,
            http::FetchRequest{"GET", path.empty() ? "/" : path, {}, std::nullopt, kMostBody},
            answer, error) ||
        answer.status != 200) {
      continue;
    }
    std::optional<IgdService> service = igd_service_in(answer.body, *origin);
    if (service) {
      return std::unique_ptr<UpnpIgd>{new UpnpIgd{*router, std::move(*service), local}};
    }
  }
  return nullptr;
}

UpnpIgd::UpnpIgd(net::Endpoint router, IgdService service, std::uint32_t local)
    : router_{router}, service_{std::move(service)}, local_{local} {
}

std::optional<std::string> UpnpIgd::call(std::string_view action, const std::string &arguments,
                                         int &error) const {
  const std::string name{action};
  const std::string body =
      "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" +
      name + " xmlns:u=\"" + service_.service_type + "\">" + arguments + "</u:" + name +
      "></s:Body></s:Envelope>";
  const std::string headers = "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" +
                              service_.service_type + "#" + name + "\"\r\n";
  http::FetchResult answer;
  std::string failure;
  if (!http::fetch(router_,
                   http::FetchRequest{"POST", service_.control_path, headers, body, kMostBody},
                   answer, failure)) {
    return std::nullopt;
  }
  if (answer.status == 200) {
    return answer.body;
  }
  if (const auto code = element(answer.body, "errorCode")) {
    std::from_chars(code->data(), code->data() + code->size(), error);
  }
  return std::nullopt;
}

std::optional<Mapping> UpnpIgd::open(std::uint16_t port, std::chrono::seconds lease) {
  int error = 0;
  const std::optional<std::string> address = call("GetExternalIPAddress", {}, error);
  const std::optional<std::string_view> text =
      address ? element(*address, "NewExternalIPAddress") : std::nullopt;
  const std::optional<std::uint32_t> external = text ? net::parse_ipv4(*text) : std::nullopt;
  if (!external || net::private_ipv4(*external)) {
    return std::nullopt;
  }
  std::uint16_t wanted = external_ != 0 ? external_ : port;
  for (int tries = 0; tries < kPortTries; ++tries) {
    const std::string arguments =
        "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(wanted) +
        "</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>" + std::to_string(port) +
        "</NewInternalPort><NewInternalClient>" + net::format_ipv4(local_) +
        "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>lucent "
        "p2p</NewPortMappingDescription><NewLeaseDuration>" +
        std::to_string(lease.count()) + "</NewLeaseDuration>";
    error = 0;
    if (call("AddPortMapping", arguments, error)) {
      external_ = wanted;
      return Mapping{{*external, wanted}, lease};
    }
    if (error != kConflictInMapping) {
      return std::nullopt;
    }
    wanted = random_port();
  }
  return std::nullopt;
}

void UpnpIgd::close(std::uint16_t /*port*/) {
  if (external_ == 0) {
    return;
  }
  int error = 0;
  static_cast<void>(call("DeletePortMapping",
                         "<NewRemoteHost></NewRemoteHost><NewExternalPort>" +
                             std::to_string(external_) +
                             "</NewExternalPort><NewProtocol>UDP</NewProtocol>",
                         error));
  external_ = 0;
}

} // namespace lucent::p2p

#include "lucent/network_interfaces.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#elif !defined(__EMSCRIPTEN__)
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
// getifaddrs reached Android's libc at API 24; older releases answer SIOCGIFCONF.
#if defined(__ANDROID__) && __ANDROID_API__ < 24
#define LUCENT_NET_IFCONF 1
#include <sys/ioctl.h>
#include <unistd.h>
#else
#include <ifaddrs.h>
#endif
#endif

namespace lucent::net {
namespace {

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
std::uint32_t host_order(const sockaddr *address) {
  return ntohl(reinterpret_cast<const sockaddr_in *>(address)->sin_addr.s_addr);
}

void set_flags(Ipv4Interface &entry, unsigned flags) {
  entry.up = (flags & IFF_UP) != 0;
  entry.running = (flags & IFF_RUNNING) != 0;
  entry.loopback = (flags & IFF_LOOPBACK) != 0;
  entry.multicast = (flags & IFF_MULTICAST) != 0;
  entry.point_to_point = (flags & IFF_POINTOPOINT) != 0;
}
#endif

#if defined(_WIN32)

std::string utf8(const wchar_t *text) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) {
    return {};
  }
  std::string out(static_cast<std::size_t>(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
  return out;
}

std::optional<std::vector<Ipv4Interface>> host_interfaces(std::string &error) {
  ULONG size = 16 * 1024;
  std::vector<unsigned char> buffer;
  ULONG result = ERROR_BUFFER_OVERFLOW;
  // The table can grow between the sizing call and the read; three tries cover that race.
  for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
    buffer.resize(size);
    result = GetAdaptersAddresses(
        AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()), &size);
  }
  if (result != NO_ERROR) {
    error = "GetAdaptersAddresses failed with " + std::to_string(result);
    return std::nullopt;
  }
  std::vector<Ipv4Interface> out;
  for (const auto *adapter = reinterpret_cast<const IP_ADAPTER_ADDRESSES *>(buffer.data());
       adapter != nullptr; adapter = adapter->Next) {
    const bool up = adapter->OperStatus != IfOperStatusDown;
    const bool running = adapter->OperStatus == IfOperStatusUp;
    const bool loopback = adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
    // Windows has no broadcast flag; a point-to-point link has no broadcast domain.
    const bool point_to_point = adapter->IfType == IF_TYPE_PPP || adapter->IfType == IF_TYPE_TUNNEL;
    const bool multicast = (adapter->Flags & IP_ADAPTER_NO_MULTICAST) == 0;
    for (const IP_ADAPTER_UNICAST_ADDRESS *at = adapter->FirstUnicastAddress; at != nullptr;
         at = at->Next) {
      const SOCKADDR *socket_address = at->Address.lpSockaddr;
      if (socket_address == nullptr || socket_address->sa_family != AF_INET) {
        continue;
      }
      Ipv4Interface entry;
      entry.name = utf8(adapter->FriendlyName);
      entry.description = utf8(adapter->Description);
      entry.address = ntohl(reinterpret_cast<const sockaddr_in *>(socket_address)->sin_addr.s_addr);
      const unsigned prefix = std::min<unsigned>(at->OnLinkPrefixLength, 32u);
      entry.netmask = prefix == 0u ? 0u : ~std::uint32_t{0} << (32u - prefix);
      entry.up = up;
      entry.running = running;
      entry.loopback = loopback;
      entry.multicast = multicast;
      entry.point_to_point = point_to_point;
      if (!loopback && !point_to_point && prefix < 31u) {
        entry.broadcast = entry.address | ~entry.netmask;
      }
      out.push_back(std::move(entry));
    }
  }
  return out;
}

#elif defined(__EMSCRIPTEN__)

std::optional<std::vector<Ipv4Interface>> host_interfaces(std::string &error) {
  error = "a browser page has no network interfaces";
  return std::nullopt;
}

#elif defined(LUCENT_NET_IFCONF)

std::optional<std::vector<Ipv4Interface>> host_interfaces(std::string &error) {
  const int probe = socket(AF_INET, SOCK_DGRAM, 0);
  if (probe < 0) {
    error = std::string("socket: ") + std::strerror(errno);
    return std::nullopt;
  }
  std::vector<ifreq> requests(64);
  ifconf table{};
  table.ifc_len = static_cast<int>(requests.size() * sizeof(ifreq));
  table.ifc_req = requests.data();
  if (ioctl(probe, SIOCGIFCONF, &table) != 0) {
    error = std::string("SIOCGIFCONF: ") + std::strerror(errno);
    close(probe);
    return std::nullopt;
  }
  std::vector<Ipv4Interface> out;
  const std::size_t count = static_cast<std::size_t>(table.ifc_len) / sizeof(ifreq);
  for (std::size_t i = 0; i < count; ++i) {
    ifreq request = requests[i];
    if (request.ifr_addr.sa_family != AF_INET) {
      continue;
    }
    Ipv4Interface entry;
    entry.name = request.ifr_name;
    entry.address = host_order(&request.ifr_addr);
    if (ioctl(probe, SIOCGIFFLAGS, &request) != 0) {
      continue;
    }
    const unsigned flags = static_cast<unsigned short>(request.ifr_flags);
    set_flags(entry, flags);
    if (ioctl(probe, SIOCGIFNETMASK, &request) == 0) {
      entry.netmask = host_order(&request.ifr_netmask);
    }
    if ((flags & IFF_BROADCAST) != 0 && ioctl(probe, SIOCGIFBRDADDR, &request) == 0) {
      entry.broadcast = host_order(&request.ifr_broadaddr);
    }
    out.push_back(std::move(entry));
  }
  close(probe);
  return out;
}

#else

std::optional<std::vector<Ipv4Interface>> host_interfaces(std::string &error) {
  ifaddrs *list = nullptr;
  if (getifaddrs(&list) != 0) {
    error = std::string("getifaddrs: ") + std::strerror(errno);
    return std::nullopt;
  }
  std::vector<Ipv4Interface> out;
  for (const ifaddrs *at = list; at != nullptr; at = at->ifa_next) {
    if (at->ifa_addr == nullptr || at->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    Ipv4Interface entry;
    entry.name = at->ifa_name;
    entry.address = host_order(at->ifa_addr);
    if (at->ifa_netmask != nullptr) {
      entry.netmask = host_order(at->ifa_netmask);
    }
    set_flags(entry, at->ifa_flags);
    if ((at->ifa_flags & IFF_BROADCAST) != 0 && at->ifa_broadaddr != nullptr &&
        at->ifa_broadaddr->sa_family == AF_INET) {
      entry.broadcast = host_order(at->ifa_broadaddr);
    }
    out.push_back(std::move(entry));
  }
  freeifaddrs(list);
  return out;
}

#endif

} // namespace

std::optional<std::vector<Ipv4Interface>> ipv4_interfaces(std::string &error) {
  return host_interfaces(error);
}

std::vector<std::uint32_t> lan_broadcasts(const std::vector<Ipv4Interface> &interfaces) {
  std::vector<std::uint32_t> out;
  for (const Ipv4Interface &entry : interfaces) {
    if (!entry.up || !entry.running || entry.loopback || !entry.broadcast) {
      continue;
    }
    if (std::find(out.begin(), out.end(), *entry.broadcast) == out.end()) {
      out.push_back(*entry.broadcast);
    }
  }
  return out;
}

} // namespace lucent::net

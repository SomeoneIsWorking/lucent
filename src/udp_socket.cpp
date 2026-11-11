#include "lucent/udp_socket.h"

#include <utility>

#include "http_socket.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <poll.h>
#endif

namespace lucent::net {
namespace {

using http::detail::is_invalid_socket;
using http::detail::kInvalidSocket;
using http::detail::native_socket;
using http::detail::Socket;

// Room for a burst while the reader catches up; the system may cap it lower.
constexpr int kBufferBytes = 4 * 1024 * 1024;

sockaddr_in socket_address(const Endpoint &endpoint) {
  sockaddr_in raw{};
  raw.sin_family = AF_INET;
  raw.sin_addr.s_addr = htonl(endpoint.address);
  raw.sin_port = htons(endpoint.port);
  return raw;
}

int set_option(Socket socket, int level, int name, int value) {
  return setsockopt(native_socket(socket), level, name, reinterpret_cast<const char *>(&value),
                    sizeof value);
}

} // namespace

std::optional<UdpSocket> UdpSocket::bind(std::uint16_t port, std::string &error) {
  if (!http::detail::initialize_socket_runtime()) {
    error = "the socket runtime did not start";
    return std::nullopt;
  }
  const Socket socket = http::detail::stored_socket(::socket(AF_INET, SOCK_DGRAM, 0));
  if (is_invalid_socket(socket)) {
    error = "cannot open a UDP socket (" + std::to_string(http::detail::last_socket_error()) + ")";
    return std::nullopt;
  }
  http::detail::set_close_on_exec(socket);
  set_option(socket, SOL_SOCKET, SO_RCVBUF, kBufferBytes);
  set_option(socket, SOL_SOCKET, SO_SNDBUF, kBufferBytes);
  const sockaddr_in any = socket_address(Endpoint{INADDR_ANY, port});
  sockaddr_in bound{};
  http::detail::SocketLength length = sizeof bound;
  if (::bind(native_socket(socket), reinterpret_cast<const sockaddr *>(&any), sizeof any) != 0 ||
      getsockname(native_socket(socket), reinterpret_cast<sockaddr *>(&bound), &length) != 0) {
    error = "cannot bind UDP port " + std::to_string(port) + " (" +
            std::to_string(http::detail::last_socket_error()) + ")";
    http::detail::close_socket(socket);
    return std::nullopt;
  }
#ifdef _WIN32
  // An ICMP port-unreachable would otherwise fail the next receive with WSAECONNRESET.
  BOOL report = FALSE;
  DWORD returned = 0;
  WSAIoctl(native_socket(socket), _WSAIOW(IOC_VENDOR, 12), &report, sizeof report, nullptr, 0,
           &returned, nullptr, nullptr);
  u_long nonblocking = 1;
  ioctlsocket(native_socket(socket), FIONBIO, &nonblocking);
#endif
  UdpSocket bound_socket{socket};
  bound_socket.port_ = ntohs(bound.sin_port);
  return bound_socket;
}

UdpSocket::~UdpSocket() {
  close();
}

UdpSocket::UdpSocket(UdpSocket &&other) noexcept
    : socket_(std::exchange(other.socket_, kInvalidSocket)), port_(other.port_) {
}

UdpSocket &UdpSocket::operator=(UdpSocket &&other) noexcept {
  if (this != &other) {
    close();
    socket_ = std::exchange(other.socket_, kInvalidSocket);
    port_ = other.port_;
  }
  return *this;
}

void UdpSocket::close() noexcept {
  http::detail::close_socket(std::exchange(socket_, kInvalidSocket));
}

bool UdpSocket::send(const Endpoint &to, std::string_view bytes) const noexcept {
  const sockaddr_in raw = socket_address(to);
#ifdef _WIN32
  const int size = static_cast<int>(bytes.size());
#else
  const std::size_t size = bytes.size();
#endif
  return sendto(native_socket(socket_), bytes.data(), size, 0,
                reinterpret_cast<const sockaddr *>(&raw),
                sizeof raw) == static_cast<std::ptrdiff_t>(bytes.size());
}

bool UdpSocket::send_within(const Endpoint &to, std::string_view bytes, int hops) const noexcept {
  int usual = 0;
  http::detail::SocketLength size = sizeof usual;
  if (getsockopt(native_socket(socket_), IPPROTO_IP, IP_TTL, reinterpret_cast<char *>(&usual),
                 &size) != 0 ||
      set_option(socket_, IPPROTO_IP, IP_TTL, hops) != 0) {
    return false;
  }
  const bool sent = send(to, bytes);
  return set_option(socket_, IPPROTO_IP, IP_TTL, usual) == 0 && sent;
}

std::optional<std::size_t> UdpSocket::receive(std::span<char> buffer, Endpoint &from,
                                              std::chrono::milliseconds wait) const noexcept {
  sockaddr_in sender{};
  http::detail::SocketLength length = sizeof sender;
#ifdef _WIN32
  const int capacity = static_cast<int>(buffer.size());
  constexpr int flags = 0;
  WSAPOLLFD watched{native_socket(socket_), POLLRDNORM, 0};
  const auto wait_for = [&watched, wait] {
    return WSAPoll(&watched, 1, static_cast<int>(wait.count())) > 0;
  };
#else
  const std::size_t capacity = buffer.size();
  constexpr int flags = MSG_DONTWAIT;
  pollfd watched{native_socket(socket_), POLLIN, 0};
  const auto wait_for = [&watched, wait] {
    return poll(&watched, 1, static_cast<int>(wait.count())) > 0;
  };
#endif
  const auto read = [&] {
    length = sizeof sender;
    return recvfrom(native_socket(socket_), buffer.data(), capacity, flags,
                    reinterpret_cast<sockaddr *>(&sender), &length);
  };
  auto got = read();
  if (got < 0) {
    if (!wait_for()) {
      return std::nullopt;
    }
    got = read();
  }
  if (got < 0) {
    return std::nullopt;
  }
  from = Endpoint{ntohl(sender.sin_addr.s_addr), ntohs(sender.sin_port)};
  return static_cast<std::size_t>(got);
}

} // namespace lucent::net

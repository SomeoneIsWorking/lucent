// A WebSocket server for one connection on a loopback socket, for tests of the client side.
#pragma once

#include "http_socket.h"
#include "lucent/http_client.h"

#include <array>
#include <cstdint>
#include <string>
#include <utility>

namespace lucent::test {

class WebSocketPeer {
public:
  WebSocketPeer() {
    namespace detail = http::detail;
    detail::initialize_socket_runtime();
    listener_ = detail::create_tcp_socket();
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool bound = ::bind(detail::native_socket(listener_),
                              reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0 &&
                       ::listen(detail::native_socket(listener_), 4) == 0;
    detail::SocketLength length = sizeof(address);
    const bool named = ::getsockname(detail::native_socket(listener_),
                                     reinterpret_cast<sockaddr *>(&address), &length) == 0;
    ok_ = bound && named;
    port_ = ntohs(address.sin_port);
  }
  ~WebSocketPeer() {
    http::detail::close_socket(client_);
    http::detail::close_socket(listener_);
  }
  WebSocketPeer(const WebSocketPeer &) = delete;
  WebSocketPeer &operator=(const WebSocketPeer &) = delete;
  WebSocketPeer(WebSocketPeer &&) = delete;
  WebSocketPeer &operator=(WebSocketPeer &&) = delete;

  std::uint16_t port() const {
    return port_;
  }
  // False once listening, accepting or a client frame went wrong (an unmasked frame among them).
  bool ok() const {
    return ok_;
  }

  // Accepts the next connection, closing the last, and answers its upgrade; a wrong accept value
  // when `honest` is false.
  void handshake(bool honest) {
    namespace detail = http::detail;
    detail::close_socket(client_);
    client_ = detail::accept_socket(listener_);
    if (detail::is_invalid_socket(client_)) {
      ok_ = false;
      return;
    }
    detail::set_socket_timeouts(client_);
    std::string request;
    while (request.find("\r\n\r\n") == std::string::npos) {
      std::array<char, 512> chunk{};
      const std::ptrdiff_t got = detail::receive_bytes(client_, chunk.data(), chunk.size());
      if (got <= 0) {
        ok_ = false;
        return;
      }
      request.append(chunk.data(), static_cast<std::size_t>(got));
    }
    const std::string marker = "Sec-WebSocket-Key: ";
    const std::size_t at = request.find(marker) + marker.size();
    const std::string key = request.substr(at, request.find("\r\n", at) - at);
    send("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
         "Sec-WebSocket-Accept: " +
         (honest ? http::websocket_accept(key) : std::string{"wrong"}) + "\r\n\r\n");
  }

  void send(const std::string &bytes) {
    http::detail::send_bytes(client_, bytes.data(), bytes.size());
  }

  // One unmasked server frame.
  void frame(std::uint8_t first, const std::string &payload) {
    std::string out;
    out.push_back(static_cast<char>(first));
    if (payload.size() < 126) {
      out.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= 0xFFFF) {
      out.push_back(static_cast<char>(126));
      out.push_back(static_cast<char>((payload.size() >> 8U) & 0xFFU));
      out.push_back(static_cast<char>(payload.size() & 0xFFU));
    } else {
      out.push_back(static_cast<char>(127));
      for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((payload.size() >> static_cast<unsigned>(shift)) & 0xFFU));
      }
    }
    send(out + payload);
  }

  // Reads one client frame, which must be masked, and returns its opcode and payload; opcode -1
  // once the client is gone.
  std::pair<int, std::string> read_frame() {
    std::array<char, 2> header{};
    if (!read(header.data(), 2)) {
      return {-1, {}};
    }
    const auto second = static_cast<std::uint8_t>(header[1]);
    if ((second & 0x80U) == 0) {
      ok_ = false;
    }
    std::uint64_t size = second & 0x7FU;
    if (size >= 126) {
      std::array<char, 8> extended{};
      const std::size_t width = size == 126 ? 2 : 8;
      if (!read(extended.data(), width)) {
        return {-1, {}};
      }
      size = 0;
      for (std::size_t index = 0; index < width; ++index) {
        size = (size << 8U) | static_cast<std::uint8_t>(extended[index]);
      }
    }
    std::array<char, 4> mask{};
    std::string payload(static_cast<std::size_t>(size), '\0');
    if (!read(mask.data(), 4) || !read(payload.data(), payload.size())) {
      return {-1, {}};
    }
    for (std::size_t i = 0; i < payload.size(); ++i) {
      payload[i] = static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^
                                     static_cast<std::uint8_t>(mask[i % 4]));
    }
    return {static_cast<std::uint8_t>(header[0]) & 0x0FU, payload};
  }

private:
  bool read(char *bytes, std::size_t size) {
    if (http::detail::is_invalid_socket(client_)) {
      return false;
    }
    while (size > 0) {
      const std::ptrdiff_t got = http::detail::receive_bytes(client_, bytes, size);
      if (got <= 0) {
        return false;
      }
      bytes += got;
      size -= static_cast<std::size_t>(got);
    }
    return true;
  }

  http::detail::Socket listener_ = http::detail::kInvalidSocket;
  http::detail::Socket client_ = http::detail::kInvalidSocket;
  std::uint16_t port_ = 0;
  bool ok_ = true;
};

} // namespace lucent::test

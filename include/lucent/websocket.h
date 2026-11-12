// lucent/websocket.h — RFC 6455 text messages over a byte stream, for either end: a client masks
// what it sends, a server requires masked frames. `websocket_upgrade` lets a `lucent::http::Server`
// handler take a connection over as a WebSocket.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "lucent/byte_stream.h"
#include "lucent/http.h"

namespace lucent::http {

enum class WebSocketRole : std::uint8_t { Client, Server };

// The Sec-WebSocket-Accept value a server answers `key` with (RFC 6455 section 4.2.2).
std::string websocket_accept(std::string_view key);
// A fresh Sec-WebSocket-Key.
std::string websocket_key();

// One open WebSocket over `stream`, which must outlive it. Text messages only; pings are answered
// while receiving.
class WebSocket {
public:
  static constexpr std::size_t kMaxMessageBytes = std::size_t{16} * 1024 * 1024;

  // `pending` holds bytes already read past the handshake.
  WebSocket(net::ByteStream &stream, std::string pending, WebSocketRole role);

  bool send_text(std::string_view text, std::string &error);
  // Waits for the next whole text message. Nothing on close, timeout, or a protocol error, after
  // which the socket is closed and `error` says which.
  std::optional<std::string> receive_text(std::string &error);
  bool open() const noexcept {
    return open_;
  }

private:
  bool send_frame(std::uint8_t opcode, std::string_view payload, std::string &error);
  bool read_exact(char *bytes, std::size_t size, std::string &error);
  bool fail(std::string &error, std::string reason);

  net::ByteStream &stream_;
  std::string pending_;
  WebSocketRole role_;
  bool open_ = true;
};

// The 101 answer to a request asking to upgrade to a WebSocket, after which the server runs
// `serve` on the connection's thread and closes the connection when it returns; a 400 for a
// request that does not ask.
Response websocket_upgrade(const Request &request, std::function<void(WebSocket &)> serve);

} // namespace lucent::http

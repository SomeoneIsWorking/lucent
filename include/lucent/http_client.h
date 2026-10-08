// lucent/http_client.h — the client side of loopback HTTP: one GET, and a WebSocket for local
// debugging protocols such as Chrome DevTools.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lucent::http {

struct FetchResult {
  int status = 0;
  std::string body;
};

// GETs `target` from 127.0.0.1:`port` over one connection. A response framed by Content-Length or
// by the connection closing is read; chunked transfer is refused and named in `error`.
bool get_loopback(std::uint16_t port, std::string_view target, FetchResult &result,
                  std::string &error);

// The Sec-WebSocket-Accept value a server answers `key` with (RFC 6455 section 4.2.2).
std::string websocket_accept(std::string_view key);

// One client WebSocket to 127.0.0.1. Text messages only; pings are answered while receiving.
class WebSocketClient {
public:
  static constexpr std::size_t kMaxMessageBytes = std::size_t{16} * 1024 * 1024;

  WebSocketClient() = default;
  ~WebSocketClient();

  WebSocketClient(const WebSocketClient &) = delete;
  WebSocketClient &operator=(const WebSocketClient &) = delete;
  WebSocketClient(WebSocketClient &&) = delete;
  WebSocketClient &operator=(WebSocketClient &&) = delete;

  // Opens the socket and completes the upgrade handshake for `path`.
  bool connect_loopback(std::uint16_t port, std::string_view path, std::string &error);
  bool send_text(std::string_view text, std::string &error);
  // Waits for the next whole text message. Nothing on close, timeout, or a protocol error, after
  // which the client is closed and `error` says which.
  std::optional<std::string> receive_text(std::string &error);
  void close() noexcept;
  bool connected() const noexcept;

private:
  bool send_frame(std::uint8_t opcode, std::string_view payload, std::string &error);
  bool read_exact(char *bytes, std::size_t size, std::string &error);
  bool fail(std::string &error, std::string reason);

  std::uintptr_t socket_ = ~std::uintptr_t{0};
  // Bytes read past the handshake's header block.
  std::string pending_;
};

} // namespace lucent::http

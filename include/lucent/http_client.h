// lucent/http_client.h — the client side of plain HTTP: one request to a loopback or LAN host, and
// a WebSocket over any byte stream: loopback for local debugging protocols such as Chrome
// DevTools, TLS to a relay through `lucent::online`.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "lucent/byte_stream.h"
#include "lucent/ipv4.h"

namespace lucent::http {

struct FetchResult {
  int status = 0;
  std::string body;
};

inline constexpr std::size_t kMaxFetchBodyBytes = std::size_t{16} * 1024 * 1024;

struct FetchRequest {
  std::string_view method = "GET";
  std::string_view target = "/";
  // Extra header lines, each ending in CRLF.
  std::string_view headers;
  std::optional<std::string_view> body;
  // A longer response body is refused.
  std::size_t max_body = kMaxFetchBodyBytes;
};

// One request over one connection to plain-HTTP `server`: loopback, or a device on the LAN such as
// a router. No TLS; a redirect is returned, never followed. A response framed by Content-Length or
// by the connection closing or chunked is read; another transfer coding is named in `error`.
bool fetch(const net::Endpoint &server, const FetchRequest &request, FetchResult &result,
           std::string &error);
// The same request over a connected `stream` (TLS from `lucent::online` for https), naming `host`
// ("name" or "name:port") in the Host header. The stream is spent: the server closes it.
bool fetch(net::ByteStream &stream, std::string_view host, const FetchRequest &request,
           FetchResult &result, std::string &error);

// GETs `target` from 127.0.0.1:`port` over one connection. A response framed by Content-Length or
// by the connection closing or chunked is read; another transfer coding is named in `error`.
bool get_loopback(std::uint16_t port, std::string_view target, FetchResult &result,
                  std::string &error);

// POSTs `body` to `target` on 127.0.0.1:`port`, read as get_loopback reads.
bool post_loopback(std::uint16_t port, std::string_view target, std::string_view body,
                   FetchResult &result, std::string &error);

// The Sec-WebSocket-Accept value a server answers `key` with (RFC 6455 section 4.2.2).
std::string websocket_accept(std::string_view key);

// One client WebSocket. Text messages only; pings are answered while receiving.
class WebSocketClient {
public:
  static constexpr std::size_t kMaxMessageBytes = std::size_t{16} * 1024 * 1024;

  WebSocketClient() = default;
  ~WebSocketClient();

  WebSocketClient(const WebSocketClient &) = delete;
  WebSocketClient &operator=(const WebSocketClient &) = delete;
  WebSocketClient(WebSocketClient &&) = delete;
  WebSocketClient &operator=(WebSocketClient &&) = delete;

  // Opens a socket to 127.0.0.1:`port` and completes the upgrade handshake for `path`.
  bool connect_loopback(std::uint16_t port, std::string_view path, std::string &error);
  // Completes the upgrade handshake for `path` over `stream`, naming `host` ("name:port").
  bool connect(std::unique_ptr<net::ByteStream> stream, std::string_view host,
               std::string_view path, std::string &error);
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

  std::unique_ptr<net::ByteStream> stream_;
  // Bytes read past the handshake's header block.
  std::string pending_;
};

} // namespace lucent::http

#include "lucent/http.h"
#include "lucent/http_client.h"

#include "http_socket.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

namespace {

namespace detail = lucent::http::detail;

int g_failures = 0;

void expect(bool condition, const char *what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << '\n';
    ++g_failures;
  }
}

// A one-connection WebSocket peer on a loopback socket.
class Peer {
public:
  Peer() {
    detail::initialize_socket_runtime();
    listener_ = detail::create_tcp_socket();
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool bound = ::bind(detail::native_socket(listener_),
                              reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0 &&
                       ::listen(detail::native_socket(listener_), 1) == 0;
    detail::SocketLength length = sizeof(address);
    const bool named = ::getsockname(detail::native_socket(listener_),
                                     reinterpret_cast<sockaddr *>(&address), &length) == 0;
    expect(bound && named, "the peer listens on loopback");
    port_ = ntohs(address.sin_port);
  }
  ~Peer() {
    detail::close_socket(client_);
    detail::close_socket(listener_);
  }
  Peer(const Peer &) = delete;
  Peer &operator=(const Peer &) = delete;
  Peer(Peer &&) = delete;
  Peer &operator=(Peer &&) = delete;

  std::uint16_t port() const {
    return port_;
  }

  // Accepts and answers the upgrade; a wrong accept value when `honest` is false.
  void handshake(bool honest) {
    client_ = detail::accept_socket(listener_);
    if (detail::is_invalid_socket(client_)) {
      expect(false, "the peer accepts the client");
      return;
    }
    detail::set_socket_timeouts(client_);
    std::string request;
    while (request.find("\r\n\r\n") == std::string::npos) {
      std::array<char, 512> chunk{};
      const std::ptrdiff_t got = detail::receive_bytes(client_, chunk.data(), chunk.size());
      if (got <= 0) {
        return;
      }
      request.append(chunk.data(), static_cast<std::size_t>(got));
    }
    const std::string marker = "Sec-WebSocket-Key: ";
    const std::size_t at = request.find(marker) + marker.size();
    const std::string key = request.substr(at, request.find("\r\n", at) - at);
    send("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
         "Sec-WebSocket-Accept: " +
         (honest ? lucent::http::websocket_accept(key) : std::string{"wrong"}) + "\r\n\r\n");
  }

  void send(const std::string &bytes) {
    detail::send_bytes(client_, bytes.data(), bytes.size());
  }

  // One unmasked server frame.
  void frame(std::uint8_t first, const std::string &payload) {
    std::string out;
    out.push_back(static_cast<char>(first));
    if (payload.size() < 126) {
      out.push_back(static_cast<char>(payload.size()));
    } else {
      out.push_back(static_cast<char>(126));
      out.push_back(static_cast<char>((payload.size() >> 8) & 0xffu));
      out.push_back(static_cast<char>(payload.size() & 0xffu));
    }
    send(out + payload);
  }

  // Reads one client frame, which must be masked, and returns its opcode and payload.
  std::pair<int, std::string> read_frame() {
    std::array<char, 2> header{};
    read(header.data(), 2);
    const auto second = static_cast<std::uint8_t>(header[1]);
    expect((second & 0x80u) != 0, "client frames are masked");
    std::array<char, 4> mask{};
    read(mask.data(), 4);
    std::string payload(second & 0x7fu, '\0');
    read(payload.data(), payload.size());
    for (std::size_t i = 0; i < payload.size(); ++i) {
      payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
    }
    return {static_cast<std::uint8_t>(header[0]) & 0x0f, payload};
  }

private:
  void read(char *bytes, std::size_t size) {
    while (size > 0) {
      const std::ptrdiff_t got = detail::receive_bytes(client_, bytes, size);
      if (got <= 0) {
        return;
      }
      bytes += got;
      size -= static_cast<std::size_t>(got);
    }
  }

  detail::Socket listener_ = detail::kInvalidSocket;
  detail::Socket client_ = detail::kInvalidSocket;
  std::uint16_t port_ = 0;
};

void test_accept_matches_rfc_example() {
  expect(lucent::http::websocket_accept("dGhlIHNhbXBsZSBub25jZQ==") ==
             "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
         "RFC 6455 accept example");
}

void test_get_reads_a_server_response() {
  lucent::http::Server server({}, [](const lucent::http::Request &request) {
    if (request.path() == "/json") {
      return lucent::http::Response::json(200, "OK", R"({"ok":true})");
    }
    return lucent::http::Response::text(404, "Not Found", "missing\n");
  });
  expect(server.start(), "server starts");
  lucent::http::FetchResult result;
  std::string error;
  expect(lucent::http::get_loopback(server.port(), "/json", result, error), "GET succeeds");
  expect(result.status == 200 && result.body == R"({"ok":true})", "GET reads status and body");
  expect(lucent::http::get_loopback(server.port(), "/nope", result, error) &&
             result.status == 404 && result.body == "missing\n",
         "GET reports a 404 as a response");
  server.stop();
  expect(!lucent::http::get_loopback(server.port(), "/json", result, error) && !error.empty(),
         "GET to a closed port fails with a reason");
}

void test_websocket_exchange() {
  Peer peer;
  std::thread server{[&peer] {
    peer.handshake(true);
    const auto [opcode, text] = peer.read_frame();
    expect(opcode == 0x1 && text == "hello", "server receives the client's text");
    peer.frame(0x89, "beat");
    const auto [pong, echoed] = peer.read_frame();
    expect(pong == 0xA && echoed == "beat", "a ping is answered with its payload");
    peer.frame(0x01, "hel");
    peer.frame(0x80, "lo");
    peer.frame(0x81, std::string(300, 'x'));
    peer.frame(0x88, "");
  }};
  lucent::http::WebSocketClient client;
  std::string error;
  expect(client.connect_loopback(peer.port(), "/devtools", error), "client connects");
  expect(client.send_text("hello", error), "client sends");
  expect(client.receive_text(error) == "hello", "fragments arrive as one message");
  expect(client.receive_text(error) == std::string(300, 'x'),
         "an extended-length frame arrives whole");
  expect(!client.receive_text(error) && !client.connected(), "a close frame ends the connection");
  server.join();
}

void test_websocket_rejects_a_wrong_accept() {
  Peer peer;
  std::thread server{[&peer] {
    peer.handshake(false);
  }};
  lucent::http::WebSocketClient client;
  std::string error;
  expect(!client.connect_loopback(peer.port(), "/", error) && !client.connected(),
         "a wrong Sec-WebSocket-Accept is refused");
  server.join();
}

} // namespace

int main() {
  test_accept_matches_rfc_example();
  test_get_reads_a_server_response();
  test_websocket_exchange();
  test_websocket_rejects_a_wrong_accept();
  if (g_failures != 0) {
    return 1;
  }
  std::cout << "http client: all checks passed\n";
  return 0;
}

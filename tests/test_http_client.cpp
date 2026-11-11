#include "lucent/http.h"
#include "lucent/http_client.h"

#include "http_socket.h"
#include "test_environment.h"
#include "websocket_peer.h"

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

void test_post_sends_its_body() {
  lucent::http::Server server({}, [](const lucent::http::Request &request) {
    if (request.method == "POST" && request.path() == "/echo") {
      return lucent::http::Response::text(200, "OK", "got " + request.body);
    }
    return lucent::http::Response::text(405, "Method Not Allowed", request.method);
  });
  expect(server.start(), "server starts");
  lucent::http::FetchResult result;
  std::string error;
  expect(lucent::http::post_loopback(server.port(), "/echo", "hello", result, error) &&
             result.status == 200 && result.body == "got hello",
         "POST delivers its body");
  expect(lucent::http::post_loopback(server.port(), "/echo", "", result, error) &&
             result.status == 200 && result.body == "got ",
         "POST with an empty body");
  expect(lucent::http::get_loopback(server.port(), "/echo", result, error) &&
             result.status == 405 && result.body == "GET",
         "GET stays a GET");
  server.stop();
}

void test_websocket_exchange() {
  lucent::test::WebSocketPeer peer;
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
  expect(peer.ok(), "the peer accepts the client and reads masked frames");
}

void test_websocket_rejects_a_wrong_accept() {
  lucent::test::WebSocketPeer peer;
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
  return lucent::test::run_main([] {
    test_accept_matches_rfc_example();
    test_get_reads_a_server_response();
    test_post_sends_its_body();
    test_websocket_exchange();
    test_websocket_rejects_a_wrong_accept();
    if (g_failures != 0) {
      return 1;
    }
    std::cout << "http client: all checks passed\n";
    return 0;
  });
}

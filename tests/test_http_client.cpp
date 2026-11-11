#include "lucent/http.h"
#include "lucent/http_client.h"

#include "http_socket.h"
#include "test_environment.h"
#include "websocket_peer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

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

// A server's response handed out a few bytes per read, then closed.
class ScriptedStream final : public lucent::net::ByteStream {
public:
  explicit ScriptedStream(std::string response) : response_{std::move(response)} {
  }
  bool send_all(std::string_view, std::string &) override {
    return true;
  }
  lucent::net::ReadOutcome read_some(std::string &buffer) override {
    if (at_ >= response_.size()) {
      return lucent::net::ReadOutcome::Closed;
    }
    const std::size_t count = std::min<std::size_t>(5, response_.size() - at_);
    buffer.append(response_, at_, count);
    at_ += count;
    return lucent::net::ReadOutcome::Data;
  }

private:
  std::string response_;
  std::size_t at_ = 0;
};

bool fetch_scripted(const std::string &response, lucent::http::FetchResult &result,
                    std::string &error, std::size_t max_body = lucent::http::kMaxFetchBodyBytes) {
  ScriptedStream stream{response};
  lucent::http::FetchRequest request;
  request.max_body = max_body;
  return lucent::http::fetch(stream, "example.test", request, result, error);
}

void test_fetch_reads_a_chunked_body() {
  const std::string head = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
  lucent::http::FetchResult result;
  std::string error;
  expect(fetch_scripted(head + "4\r\nWiki\r\n7;note=x\r\npedia i\r\nB\r\nn \r\nchunks.\r\n0\r\n"
                               "Expires: never\r\n\r\n",
                        result, error) &&
             result.status == 200 && result.body == "Wikipedia in \r\nchunks.",
         "a chunked body arrives whole, extensions and trailers skipped");
  expect(!fetch_scripted(head + "4\r\nWiki\r\n", result, error) && !error.empty(),
         "a chunked body cut off before its last chunk fails");
  expect(!fetch_scripted(head + "zz\r\nWiki\r\n0\r\n\r\n", result, error),
         "a chunk size that is not hex fails");
  expect(!fetch_scripted(head + "4\r\nWikiX\r\n0\r\n\r\n", result, error),
         "a chunk longer than its size fails");
  expect(!fetch_scripted(head + "8\r\nWikipedi\r\n0\r\n\r\n", result, error, 4),
         "a chunked body past the cap fails");
  expect(!fetch_scripted("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\nxx", result, error) &&
             error.find("gzip") != std::string::npos,
         "another transfer coding is named");
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
    test_fetch_reads_a_chunked_body();
    test_websocket_exchange();
    test_websocket_rejects_a_wrong_accept();
    if (g_failures != 0) {
      return 1;
    }
    std::cout << "http client: all checks passed\n";
    return 0;
  });
}

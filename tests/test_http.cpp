#include "lucent/http.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

int g_failures = 0;

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " << #condition << "\n";           \
      ++g_failures;                                                                                \
    }                                                                                              \
  } while (0)

bool send_all(int socket, std::string_view bytes) {
  while (!bytes.empty()) {
    const ssize_t sent = send(socket, bytes.data(), bytes.size(), 0);
    if (sent <= 0)
      return false;
    bytes.remove_prefix(static_cast<std::size_t>(sent));
  }
  return true;
}

std::string request(std::uint16_t port, std::string_view wire) {
  const int client = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(client >= 0);
  if (client < 0)
    return {};

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK(connect(client, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0);
  if (!send_all(client, wire)) {
    CHECK(false);
    close(client);
    return {};
  }
  shutdown(client, SHUT_WR);

  std::string response;
  char block[2048];
  for (;;) {
    const ssize_t count = recv(client, block, sizeof(block), 0);
    if (count <= 0)
      break;
    response.append(block, static_cast<std::size_t>(count));
  }
  close(client);
  return response;
}

std::string_view body(std::string_view response) {
  const std::size_t separator = response.find("\r\n\r\n");
  return separator == std::string_view::npos ? std::string_view{} : response.substr(separator + 4);
}

void test_form_decoder() {
  std::vector<lucent::http::FormField> fields;
  std::string error;
  CHECK(lucent::http::parse_form_urlencoded("buttons=A%2CSTART&name=Marcus+Fenix", fields, error));
  CHECK(fields.size() == 2);
  CHECK(fields[0].name == "buttons");
  CHECK(fields[0].value == "A,START");
  CHECK(fields[1].value == "Marcus Fenix");

  fields.clear();
  CHECK(!lucent::http::parse_form_urlencoded("broken=%xz", fields, error));
  CHECK(error == "invalid percent-encoding in form data");
}

void test_server_transport_and_concurrency() {
  std::atomic<int> handler_calls{0};
  std::promise<void> slow_entered;
  std::future<void> slow_entered_future = slow_entered.get_future();
  std::promise<void> release_slow;
  std::shared_future<void> release_slow_future = release_slow.get_future().share();

  lucent::http::ServerOptions options;
  options.port = 0;
  options.max_body_bytes = 32;
  options.max_connections = 4;
  lucent::http::Server server(options, [&](const lucent::http::Request &incoming) {
    handler_calls.fetch_add(1);
    if (incoming.path() == "/slow") {
      slow_entered.set_value();
      release_slow_future.wait();
      return lucent::http::Response::text(200, "OK", "slow done");
    }
    if (incoming.path() == "/echo") {
      return lucent::http::Response::json(
          200, "OK", incoming.method + " " + std::string(incoming.query()) + " " + incoming.body);
    }
    return lucent::http::Response::text(404, "Not Found", "missing");
  });

  CHECK(server.start());
  CHECK(server.running());
  CHECK(server.port() != 0);

  const std::string echo =
      request(server.port(),
              "POST /echo?probe=1 HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 5\r\n\r\nhello");
  CHECK(echo.starts_with("HTTP/1.1 200 OK\r\n"));
  CHECK(echo.find("Content-Type: application/json\r\n") != std::string::npos);
  CHECK(body(echo) == "POST probe=1 hello");

  const int before_invalid = handler_calls.load();
  const std::string invalid = request(
      server.port(), "POST /echo HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: nope\r\n\r\n");
  CHECK(invalid.starts_with("HTTP/1.1 400 Bad Request\r\n"));
  CHECK(body(invalid) == "invalid Content-Length\n");
  CHECK(handler_calls.load() == before_invalid);

  const std::string oversized = request(
      server.port(), "POST /echo HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 33\r\n\r\n");
  CHECK(oversized.starts_with("HTTP/1.1 413 Content Too Large\r\n"));
  CHECK(body(oversized) == "request body is too large\n");
  CHECK(handler_calls.load() == before_invalid);

  std::string slow_response;
  std::thread slow_client([&] {
    slow_response = request(server.port(), "GET /slow HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  });
  slow_entered_future.wait();
  const std::string fast_response =
      request(server.port(), "GET /echo?fast=1 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  CHECK(fast_response.starts_with("HTTP/1.1 200 OK\r\n"));
  CHECK(body(fast_response) == "GET fast=1 ");
  release_slow.set_value();
  slow_client.join();
  CHECK(body(slow_response) == "slow done");

  server.stop();
  CHECK(!server.running());
  CHECK(server.port() == 0);
}

} // namespace

int main() {
  test_form_decoder();
  test_server_transport_and_concurrency();
  if (g_failures == 0) {
    std::cout << "all HTTP tests passed\n";
  } else {
    std::cerr << g_failures << " failure(s)\n";
  }
  return g_failures == 0 ? 0 : 1;
}

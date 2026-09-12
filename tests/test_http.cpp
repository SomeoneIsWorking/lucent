#include "lucent/http.h"

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <semaphore>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

int g_failures = 0;

#ifdef _WIN32
using ClientSocket = SOCKET;
using SocketLength = int;
#else
using ClientSocket = int;
using SocketLength = socklen_t;
#endif

bool valid_client(ClientSocket client) {
#ifdef _WIN32
  return client != INVALID_SOCKET;
#else
  return client >= 0;
#endif
}

void close_client(ClientSocket client) {
#ifdef _WIN32
  closesocket(client);
#else
  close(client);
#endif
}

void shutdown_client_write(ClientSocket client) {
#ifdef _WIN32
  shutdown(client, SD_SEND);
#else
  shutdown(client, SHUT_WR);
#endif
}

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " << #condition << "\n";           \
      ++g_failures;                                                                                \
    }                                                                                              \
  } while (0)

bool send_all(ClientSocket client, std::string_view bytes) {
  while (!bytes.empty()) {
    auto bounded =
        (std::min)(bytes.size(), static_cast<std::size_t>((std::numeric_limits<int>::max)()));
    auto sent = send(client, bytes.data(), static_cast<int>(bounded), 0);
    if (sent <= 0) {
      return false;
    }
    bytes.remove_prefix(static_cast<std::size_t>(sent));
  }
  return true;
}

std::optional<std::string> try_request(std::uint16_t port, in_addr address_value,
                                       std::string_view wire) {
  ClientSocket client = socket(AF_INET, SOCK_STREAM, 0);
  if (!valid_client(client)) {
    return std::nullopt;
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr = address_value;
  if (connect(client, reinterpret_cast<const sockaddr *>(&address),
              static_cast<SocketLength>(sizeof(address))) != 0) {
    close_client(client);
    return std::nullopt;
  }
  if (!send_all(client, wire)) {
    close_client(client);
    return std::nullopt;
  }
  shutdown_client_write(client);

  std::string response;
  char block[2048];
  for (;;) {
    auto count = recv(client, block, static_cast<int>(sizeof(block)), 0);
    if (count <= 0) {
      break;
    }
    response.append(block, static_cast<std::size_t>(count));
  }
  close_client(client);
  return response;
}

std::string request(std::uint16_t port, std::string_view wire) {
  in_addr loopback{};
  loopback.s_addr = htonl(INADDR_LOOPBACK);
  auto response = try_request(port, loopback, wire);
  CHECK(response.has_value());
  return response.value_or(std::string{});
}

std::optional<in_addr> local_network_address() {
#ifdef _WIN32
  ULONG bytes = 15 * 1024;
  std::vector<unsigned char> storage(bytes);
  ULONG status = ERROR_BUFFER_OVERFLOW;
  for (int attempt = 0; attempt < 2; ++attempt) {
    status = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST, nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data()), &bytes);
    if (status != ERROR_BUFFER_OVERFLOW) {
      break;
    }
    storage.resize(bytes);
  }
  if (status != NO_ERROR) {
    return std::nullopt;
  }
  auto *adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data());
  for (IP_ADAPTER_ADDRESSES *adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
      continue;
    }
    for (IP_ADAPTER_UNICAST_ADDRESS *entry = adapter->FirstUnicastAddress; entry != nullptr;
         entry = entry->Next) {
      if (entry->Address.lpSockaddr != nullptr && entry->Address.lpSockaddr->sa_family == AF_INET) {
        auto *address = reinterpret_cast<sockaddr_in *>(entry->Address.lpSockaddr);
        if (address->sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
          return address->sin_addr;
        }
      }
    }
  }
  return std::nullopt;
#else
  ifaddrs *interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0) {
    return std::nullopt;
  }

  std::optional<in_addr> result;
  for (const ifaddrs *interface = interfaces; interface != nullptr;
       interface = interface->ifa_next) {
    if (interface->ifa_addr == nullptr || interface->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    const auto *address = reinterpret_cast<const sockaddr_in *>(interface->ifa_addr);
    if (address->sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
      result = address->sin_addr;
      break;
    }
  }
  freeifaddrs(interfaces);
  return result;
#endif
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
  std::binary_semaphore slow_entered{0};
  std::binary_semaphore release_slow{0};

  lucent::http::ServerOptions options;
  options.port = 0;
  options.max_body_bytes = 32;
  options.max_connections = 4;
  lucent::http::Server server(options, [&](const lucent::http::Request &incoming) {
    handler_calls.fetch_add(1);
    if (incoming.path() == "/slow") {
      slow_entered.release();
      release_slow.acquire();
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
  slow_entered.acquire();
  const std::string fast_response =
      request(server.port(), "GET /echo?fast=1 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  CHECK(fast_response.starts_with("HTTP/1.1 200 OK\r\n"));
  CHECK(body(fast_response) == "GET fast=1 ");
  release_slow.release();
  slow_client.join();
  CHECK(body(slow_response) == "slow done");

  server.stop();
  CHECK(!server.running());
  CHECK(server.port() == 0);
}

void test_local_network_scope_is_explicit() {
  const auto network_address = local_network_address();
  if (!network_address.has_value()) {
    std::cout << "SKIP local-network HTTP discriminator: no non-loopback IPv4 interface\n";
    return;
  }

  const std::string wire = "GET /share HTTP/1.1\r\nHost: lan\r\n\r\n";
  lucent::http::ServerOptions loopback_options;
  loopback_options.port = 0;
  lucent::http::Server loopback(loopback_options, [](const lucent::http::Request &) {
    return lucent::http::Response::text(200, "OK", "loopback");
  });
  CHECK(loopback.start());
  CHECK(!try_request(loopback.port(), *network_address, wire).has_value());
  loopback.stop();

  lucent::http::ServerOptions network_options;
  network_options.port = 0;
  network_options.listen_scope = lucent::http::ListenScope::LocalNetwork;
  lucent::http::Server network(network_options, [](const lucent::http::Request &) {
    return lucent::http::Response::text(200, "OK", "shared");
  });
  CHECK(network.start());
  const auto response = try_request(network.port(), *network_address, wire);
  CHECK(response.has_value());
  CHECK(response.has_value() && body(*response) == "shared");
  network.stop();
}

void test_file_response_streams_exact_bytes() {
  const auto path = std::filesystem::current_path() / "http-file-response.bin";
  std::string expected(std::size_t{128} * 1024, '\0');
  for (std::size_t index = 0; index < expected.size(); ++index) {
    expected[index] = static_cast<char>(index % 251);
  }
  {
    std::ofstream file(path, std::ios::binary);
    file.write(expected.data(), static_cast<std::streamsize>(expected.size()));
    CHECK(file.good());
  }
  lucent::http::Server server({}, [path](const lucent::http::Request &) {
    return lucent::http::Response::file(200, "OK", "application/zip", path.string());
  });
  CHECK(server.start());
  const auto response = request(server.port(), "GET /pack HTTP/1.1\r\nHost: localhost\r\n\r\n");
  CHECK(response.starts_with("HTTP/1.1 200 OK\r\n"));
  CHECK(body(response) == expected);
  server.stop();
  std::error_code error;
  std::filesystem::remove(path, error);
  CHECK(!error);
}

} // namespace

int main() {
  test_form_decoder();
  test_server_transport_and_concurrency();
  test_local_network_scope_is_explicit();
  test_file_response_streams_exact_bytes();
  if (g_failures == 0) {
    std::cout << "all HTTP tests passed\n";
  } else {
    std::cerr << g_failures << " failure(s)\n";
  }
  return g_failures == 0 ? 0 : 1;
}

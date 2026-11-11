// lucent_lobby_server [--port N]: a LobbyBoard on every IPv4 interface until it is asked to stop.
// TLS, if wanted, is a reverse proxy in front of it.
#include "lucent/http.h"
#include "lucent/log.h"
#include "lucent/p2p/lobby_board.h"
#include "lucent/stop_signals.h"

#include <charconv>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

namespace {

constexpr std::uint16_t kDefaultPort = 8077;

int serve(int argc, char **argv) {
  std::uint16_t port = kDefaultPort;
  if (argc == 3 && std::string_view{argv[1]} == "--port") {
    const std::string_view digits{argv[2]};
    const auto [end, problem] = std::from_chars(digits.data(), digits.data() + digits.size(), port);
    if (problem != std::errc{} || end != digits.data() + digits.size() || port == 0) {
      lucent::log(lucent::Level::Error, "lobby", "not a port: " + std::string{digits});
      return 2;
    }
  } else if (argc != 1) {
    lucent::log(lucent::Level::Error, "lobby", "usage: lucent_lobby_server [--port N]");
    return 2;
  }
  // Before the server's threads start.
  lucent::StopSignals stop;

  lucent::p2p::LobbyBoard board;
  lucent::http::ServerOptions options;
  options.port = port;
  options.listen_scope = lucent::http::ListenScope::LocalNetwork;
  options.max_body_bytes = std::size_t{16} * 1024;
  options.max_connections = 32;
  options.backlog = 64;
  lucent::http::Server server{options, [&board](const lucent::http::Request &request) {
                                return board.handle(request);
                              }};
  if (!server.start()) {
    lucent::log(lucent::Level::Error, "lobby", "cannot listen on port " + std::to_string(port));
    return 1;
  }
  lucent::log(lucent::Level::Info, "lobby", "serving lobbies on port " + std::to_string(port));
  lucent::log(lucent::Level::Info, "lobby", "stopping on " + stop.wait());
  server.stop();
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  try {
    return serve(argc, argv);
  } catch (const std::exception &error) {
    lucent::log(lucent::Level::Error, "lobby", std::string{"stopped: "} + error.what());
    return 1;
  }
}

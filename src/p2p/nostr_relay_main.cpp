// lucent_nostr_relay [--port N] [--lan]: a Nostr relay for a lab with no internet, on loopback (or
// every IPv4 interface with --lan) until it is asked to stop.
#include "lucent/http.h"
#include "lucent/log.h"
#include "lucent/p2p/nostr_relay.h"
#include "lucent/stop_signals.h"
#include "lucent/websocket.h"

#include <charconv>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

namespace {

constexpr std::uint16_t kDefaultPort = 7447;

int serve(int argc, char **argv) {
  std::uint16_t port = kDefaultPort;
  bool lan = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (argument == "--lan") {
      lan = true;
    } else if (argument == "--port" && index + 1 < argc) {
      const std::string_view digits{argv[++index]};
      const auto [end, problem] =
          std::from_chars(digits.data(), digits.data() + digits.size(), port);
      if (problem != std::errc{} || end != digits.data() + digits.size() || port == 0) {
        lucent::log(lucent::Level::Error, "nostr", "not a port: " + std::string{digits});
        return 2;
      }
    } else {
      lucent::log(lucent::Level::Error, "nostr", "usage: lucent_nostr_relay [--port N] [--lan]");
      return 2;
    }
  }
  // Before the server's threads start.
  lucent::StopSignals stop;

  lucent::p2p::nostr::RelayStore store;
  lucent::http::ServerOptions options;
  options.port = port;
  options.listen_scope =
      lan ? lucent::http::ListenScope::LocalNetwork : lucent::http::ListenScope::Loopback;
  options.max_connections = 64;
  options.backlog = 64;
  lucent::http::Server server{options, [&store](const lucent::http::Request &request) {
                                return lucent::http::websocket_upgrade(
                                    request, [&store](lucent::http::WebSocket &socket) {
                                      store.serve(socket);
                                    });
                              }};
  if (!server.start()) {
    lucent::log(lucent::Level::Error, "nostr", "cannot listen on port " + std::to_string(port));
    return 1;
  }
  lucent::log(lucent::Level::Info, "nostr", "relay on port " + std::to_string(port));
  lucent::log(lucent::Level::Info, "nostr", "stopping on " + stop.wait());
  server.stop();
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  try {
    return serve(argc, argv);
  } catch (const std::exception &error) {
    lucent::log(lucent::Level::Error, "nostr", std::string{"stopped: "} + error.what());
    return 1;
  }
}

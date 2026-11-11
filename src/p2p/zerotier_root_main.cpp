// lucent_zerotier_root <storage> <address/port> <roots file>: a ZeroTier root for a lab with no
// internet. Makes a root identity in `storage`, writes the signed root set naming it at that
// endpoint for the lab's nodes, and runs the root until it is asked to stop.
#include "lucent/log.h"
#include "lucent/p2p/zerotier.h"
#include "lucent/stop_signals.h"

#include <charconv>
#include <cstdint>
#include <exception>
#include <fstream>
#include <string>
#include <string_view>

namespace {

int serve(int argc, char **argv) {
  if (argc != 4) {
    lucent::log(lucent::Level::Error, "zerotier",
                "usage: lucent_zerotier_root <storage> <address/port> <roots file>");
    return 2;
  }
  const std::filesystem::path storage = argv[1];
  const std::string endpoint = argv[2];
  const std::string_view digits = std::string_view{endpoint}.substr(endpoint.rfind('/') + 1);
  std::uint16_t port = 0;
  const auto [end, problem] = std::from_chars(digits.data(), digits.data() + digits.size(), port);
  if (endpoint.find('/') == std::string::npos || problem != std::errc{} ||
      end != digits.data() + digits.size() || port == 0) {
    lucent::log(lucent::Level::Error, "zerotier", "not an address/port: " + endpoint);
    return 2;
  }
  // Before the node's threads start.
  lucent::StopSignals stop;

  std::string error;
  const std::optional<std::string> roots = lucent::p2p::make_root(storage, endpoint, error);
  if (!roots) {
    lucent::log(lucent::Level::Error, "zerotier", "no root: " + error);
    return 1;
  }
  std::ofstream out{argv[3], std::ios::binary};
  out << *roots;
  out.close();
  if (!out) {
    lucent::log(lucent::Level::Error, "zerotier", std::string{"cannot write "} + argv[3]);
    return 1;
  }
  // A root joins no network of its own; the port only names one.
  const auto node = lucent::p2p::ZeroTierNode::start(
      {.storage = storage, .roots = *roots, .port = port, .network_port = port}, error);
  if (!node) {
    lucent::log(lucent::Level::Error, "zerotier", "no root node: " + error);
    return 1;
  }
  lucent::log(lucent::Level::Info, "zerotier", "root running at " + endpoint);
  lucent::log(lucent::Level::Info, "zerotier", "stopping on " + stop.wait());
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  try {
    return serve(argc, argv);
  } catch (const std::exception &error) {
    lucent::log(lucent::Level::Error, "zerotier", std::string{"stopped: "} + error.what());
    return 1;
  }
}

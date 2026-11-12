// One player of the ZeroTier test: a host printing its code, a friend joining by it, or a stranger
// with the code's public half only. Each prints one line and then the link key it agreed on;
// `test_zerotier` runs three of them through a lab root. Portable, so a Windows build can play one.
#include "lucent/p2p/connector.h"
#include "lucent/p2p/crypto.h"
#include "lucent/p2p/reliable_link.h"
#include "lucent/p2p/zerotier.h"
#include "lucent/text.h"

#include <cassert>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace lucent::p2p;
using namespace std::chrono_literals;

constexpr std::uint16_t kNetworkPort = 47000;
using Clock = std::chrono::steady_clock;

long long ms_since(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

std::string read_file(const std::filesystem::path &path) {
  std::ifstream in{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{in}, {}};
}

std::unique_ptr<ZeroTierNode> node(const std::filesystem::path &dir, const char *name,
                                   std::uint16_t port) {
  std::string error;
  const Clock::time_point begun = Clock::now();
  auto started = ZeroTierNode::start(
      ZeroTierConfig{dir / name, read_file(dir / "roots"), port, kNetworkPort}, error);
  if (!started) {
    std::fprintf(stderr, "%s: %s\n", name, error.c_str());
    std::exit(1);
  }
  if (!started->wait_ready(lucent::StopToken{}, 60s)) {
    std::fprintf(stderr, "%s: never on its network\n", name);
    std::exit(1);
  }
  std::fprintf(stderr, "%s: on the network %lld ms after starting\n", name, ms_since(begun));
  return started;
}

LinkCaps test_caps() {
  LinkCaps caps{};
  caps[1] = 16;
  return caps;
}

ConnectorConfig zerotier_only(const ZeroTierPaths &zerotier) {
  ConnectorConfig config;
  config.direct.stun_servers = "";
  config.direct.router = false;
  config.direct.interfaces = std::vector<lucent::net::Ipv4Interface>{};
  config.zerotier = &zerotier;
  config.prefixes = {"TST", "TSR"};
  config.label = "lucent-test";
  return config;
}

// Prints the code, then the link key the connection agreed on.
int run_host(const std::filesystem::path &dir, std::uint16_t port) {
  const auto zerotier = node(dir, "host", port);
  Connector connector{zerotier_only(*zerotier)};
  for (int round = 0; round < 600 && connector.view().step != Connector::Step::Inviting; ++round) {
    std::this_thread::sleep_for(100ms);
  }
  std::printf("%s\n", connector.view().code.c_str());
  std::fflush(stdout);
  for (int round = 0; round < 1200 && connector.view().step != Connector::Step::Connected;
       ++round) {
    std::this_thread::sleep_for(100ms);
  }
  const auto connection = connector.take();
  if (!connection || connection->path->backend() != Backend::ZeroTier) {
    std::fprintf(stderr, "host: no ZeroTier connection\n");
    return 1;
  }
  ReliableLink link{*connection->path,
                    connection->peer,
                    {connection->keys.key(kLinkPurpose, Role::Host),
                     connection->keys.key(kLinkPurpose, Role::Friend)},
                    test_caps()};
  std::optional<LinkMessage> message;
  for (int round = 0; round < 300 && !message; ++round) {
    link.pump(100ms);
    message = link.take();
  }
  // Long enough for the friend to hear the acknowledgement.
  for (int round = 0; round < 30; ++round) {
    link.pump(100ms);
  }
  if (!message || message->body != "hi") {
    std::fprintf(stderr, "host: the friend's message never came\n");
    return 1;
  }
  std::printf("%s\n", lucent::text::hex(connection->keys.key(kLinkPurpose, Role::Host)).c_str());
  return 0;
}

// Someone who knows the host's node and network, which are public, but not the code's secret.
int run_stranger(const std::filesystem::path &dir, std::uint16_t port, const std::string &code) {
  const auto zerotier = node(dir, "stranger", port);
  ConnectorConfig config = zerotier_only(*zerotier);
  config.patience.friend_tries = 20s;
  CodeProblem problem{};
  std::optional<Invite> invite = read_invite(code, config.prefixes, problem);
  assert(invite);
  invite->secret = crypto::random_bytes(kSecretBytes);
  Connector connector{std::move(config), std::move(*invite)};
  for (int round = 0; round < 250 && connector.view().step != Connector::Step::Connected &&
                      connector.view().step != Connector::Step::Failed;
       ++round) {
    std::this_thread::sleep_for(100ms);
  }
  std::printf("%s\n", connector.view().step == Connector::Step::Connected ? "connected" : "none");
  return 0;
}

int run_friend(const std::filesystem::path &dir, std::uint16_t port, const std::string &code) {
  const auto zerotier = node(dir, "friend", port);
  ConnectorConfig config = zerotier_only(*zerotier);
  CodeProblem problem{};
  std::optional<Invite> invite = read_invite(code, config.prefixes, problem);
  assert(invite && invite->descriptor.zerotier);
  const Clock::time_point pasted = Clock::now();
  Connector connector{std::move(config), std::move(*invite)};
  for (int round = 0; round < 1200 && connector.view().step != Connector::Step::Connected;
       ++round) {
    std::this_thread::sleep_for(10ms);
  }
  std::fprintf(stderr, "friend: connected %lld ms after pasting the code\n", ms_since(pasted));
  const auto connection = connector.take();
  if (!connection || connection->path->backend() != Backend::ZeroTier) {
    std::fprintf(stderr, "friend: no ZeroTier connection\n");
    return 1;
  }
  ReliableLink link{*connection->path,
                    connection->peer,
                    {connection->keys.key(kLinkPurpose, Role::Friend),
                     connection->keys.key(kLinkPurpose, Role::Host)},
                    test_caps()};
  link.send(1, "hi");
  for (int round = 0; round < 300 && !link.flushed(); ++round) {
    link.pump(100ms);
  }
  if (!link.flushed()) {
    std::fprintf(stderr, "friend: the host never acknowledged\n");
    return 1;
  }
  std::printf("%s\n", lucent::text::hex(connection->keys.key(kLinkPurpose, Role::Host)).c_str());
  return 0;
}

} // namespace

// <host|friend|stranger> <lab folder> <node port> [code]
int main(int argc, char **argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s host|friend|stranger <folder> <port> [code]\n", argv[0]);
    return 2;
  }
  const std::string role = argv[1];
  const std::filesystem::path dir = argv[2];
  const std::string_view digits = argv[3];
  std::uint16_t port = 0;
  if (std::from_chars(digits.data(), digits.data() + digits.size(), port).ec != std::errc{}) {
    std::fprintf(stderr, "not a port: %s\n", argv[3]);
    return 2;
  }
  if (role == "host") {
    return run_host(dir, port);
  }
  if (argc < 5) {
    std::fprintf(stderr, "%s needs the host's code\n", role.c_str());
    return 2;
  }
  return role == "stranger" ? run_stranger(dir, port, argv[4]) : run_friend(dir, port, argv[4]);
}

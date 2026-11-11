// Two processes connect by a code over embedded ZeroTier alone, through `lucent_zerotier_root` in
// a third: one libzt node per process. ctest runs it in a network namespace with a dummy interface,
// as ZeroTier sends nothing over loopback.
#include "lucent/p2p/connector.h"
#include "lucent/p2p/crypto.h"
#include "lucent/p2p/reliable_link.h"
#include "lucent/p2p/zerotier.h"
#include "lucent/text.h"

#include <array>
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace {

using namespace lucent::p2p;
using namespace std::chrono_literals;

constexpr const char *kLabAddress = "10.77.0.1";
constexpr std::uint16_t kRootPort = 39993;
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
int run_host(const std::filesystem::path &dir) {
  const auto zerotier = node(dir, "host", kRootPort + 1);
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
int run_stranger(const std::filesystem::path &dir, const std::string &code) {
  const auto zerotier = node(dir, "stranger", kRootPort + 3);
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

int run_friend(const std::filesystem::path &dir, const std::string &code) {
  const auto zerotier = node(dir, "friend", kRootPort + 2);
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

// A child running this test as one role, its standard output read through `output`.
struct Child {
  pid_t pid = 0;
  int output = -1;

  int wait() {
    if (output >= 0) {
      close(output);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  }
};

Child spawn(const std::string &program, const std::vector<std::string> &arguments, bool read) {
  std::vector<std::string> words{program};
  words.insert(words.end(), arguments.begin(), arguments.end());
  std::vector<char *> argv;
  argv.reserve(words.size() + 1);
  for (std::string &word : words) {
    argv.push_back(word.data());
  }
  argv.push_back(nullptr);
  std::array<int, 2> pipe_ends{-1, -1};
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  if (read) {
    const int piped = pipe(pipe_ends.data());
    assert(piped == 0);
    posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_ends[0]);
  }
  Child child;
  const int spawned =
      posix_spawn(&child.pid, program.c_str(), &actions, nullptr, argv.data(), environ);
  assert(spawned == 0);
  posix_spawn_file_actions_destroy(&actions);
  if (read) {
    close(pipe_ends[1]);
    child.output = pipe_ends[0];
  }
  return child;
}

std::string first_line(int output) {
  std::string text;
  char byte = 0;
  while (::read(output, &byte, 1) == 1 && byte != '\n') {
    text += byte;
  }
  return text;
}

int run_test(const std::string &self, const std::string &root_program) {
  const std::filesystem::path dir =
      std::filesystem::current_path() / ("zerotier-test-" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  const std::filesystem::path roots = dir / "roots";
  Child root = spawn(root_program,
                     {(dir / "root").string(),
                      std::string{kLabAddress} + "/" + std::to_string(kRootPort), roots.string()},
                     false);
  // The root writes its root set before its node starts.
  for (int tries = 0; tries < 100 && read_file(roots).empty(); ++tries) {
    std::this_thread::sleep_for(100ms);
  }
  assert(!read_file(roots).empty());
  Child host = spawn(self, {"host", dir.string()}, true);
  const std::string code = first_line(host.output);
  assert(code.starts_with("TST"));
  Child stranger = spawn(self, {"stranger", dir.string(), code}, true);
  Child other = spawn(self, {"friend", dir.string(), code}, true);
  const std::string friend_key = first_line(other.output);
  const std::string stranger_result = first_line(stranger.output);
  const int stranger_status = stranger.wait();
  const std::string host_key = first_line(host.output);
  const int friend_status = other.wait();
  const int host_status = host.wait();
  kill(root.pid, SIGTERM);
  const int root_status = root.wait();
  std::filesystem::remove_all(dir);
  assert(friend_status == 0 && host_status == 0 && root_status == 0);
  assert(!host_key.empty() && host_key == friend_key);
  assert(stranger_status == 0 && stranger_result == "none");
  std::puts("two processes connected over ZeroTier alone and agree on keys; a stranger did not");
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  const std::string self = std::filesystem::canonical("/proc/self/exe").string();
  const std::string role = argv[1];
  if (role == "--root") {
    return run_test(self, argv[2]);
  }
  const std::filesystem::path dir = argv[2];
  if (role == "host") {
    return run_host(dir);
  }
  if (role == "stranger") {
    return run_stranger(dir, argv[3]);
  }
  return run_friend(dir, argv[3]);
}

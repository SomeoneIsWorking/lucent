// Two processes connect by a code over embedded ZeroTier alone, through a lab root in a third:
// one libzt node per process. ctest runs it in a network namespace with a dummy interface, as
// ZeroTier sends nothing over loopback.
#include "lucent/p2p/connector.h"
#include "lucent/p2p/crypto.h"
#include "lucent/p2p/reliable_link.h"

#include <ZeroTierSockets.h>

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

std::string read_file(const std::filesystem::path &path) {
  std::ifstream in{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{in}, {}};
}

void write_file(const std::filesystem::path &path, std::string_view bytes) {
  std::ofstream{path, std::ios::binary}.write(bytes.data(),
                                              static_cast<std::streamsize>(bytes.size()));
}

std::string hex(const crypto::Bytes &bytes) {
  std::string out;
  for (const std::uint8_t byte : bytes) {
    std::array<char, 3> two{};
    std::snprintf(two.data(), two.size(), "%02x", byte);
    out += two.data();
  }
  return out;
}

// A root identity and the signed root set naming it at the lab address.
void make_root(const std::filesystem::path &dir) {
  std::array<char, ZTS_ID_STR_BUF_LEN> key{};
  unsigned int length = key.size();
  const int made = zts_id_new(key.data(), &length);
  assert(made == ZTS_ERR_OK);
  const std::string secret{key.data()};
  std::string identity = secret.substr(0, secret.rfind(':'));
  std::string endpoint = std::string{kLabAddress} + "/" + std::to_string(kRootPort);
  zts_root_set_t roots{};
  roots.public_id_str[0] = identity.data();
  roots.endpoint_ip_str[0][0] = endpoint.data();
  std::array<char, ZTS_STORE_DATA_LEN> set{};
  std::array<char, ZTS_STORE_DATA_LEN> previous{};
  std::array<char, ZTS_STORE_DATA_LEN> current{};
  unsigned int set_length = 0;
  unsigned int previous_length = 0;
  unsigned int current_length = 0;
  const int signed_set =
      zts_util_sign_root_set(set.data(), &set_length, previous.data(), &previous_length,
                             current.data(), &current_length, 1, 1, &roots);
  assert(signed_set == ZTS_ERR_OK);
  std::filesystem::create_directories(dir / "root");
  write_file(dir / "root" / "identity.secret", secret);
  write_file(dir / "root" / "identity.public", identity);
  write_file(dir / "roots", {set.data(), set_length});
}

std::unique_ptr<ZeroTierNode> node(const std::filesystem::path &dir, const char *name,
                                   std::uint16_t port) {
  std::string error;
  auto started =
      ZeroTierNode::start(ZeroTierConfig{dir / name, read_file(dir / "roots"), port}, error);
  if (!started) {
    std::fprintf(stderr, "%s: %s\n", name, error.c_str());
    std::exit(1);
  }
  if (!started->wait_online(std::stop_token{}, 60s)) {
    std::fprintf(stderr, "%s: never online\n", name);
    std::exit(1);
  }
  return started;
}

LinkCaps test_caps() {
  LinkCaps caps{};
  caps[1] = 16;
  return caps;
}

ConnectorConfig zerotier_only(const ZeroTierNode &zerotier) {
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
  std::printf("%s\n", hex(connection->keys.key(kLinkPurpose, Role::Host)).c_str());
  return 0;
}

int run_friend(const std::filesystem::path &dir, const std::string &code) {
  const auto zerotier = node(dir, "friend", kRootPort + 2);
  ConnectorConfig config = zerotier_only(*zerotier);
  CodeProblem problem{};
  std::optional<Invite> invite = read_invite(code, config.prefixes, problem);
  assert(invite && invite->descriptor.zerotier);
  Connector connector{std::move(config), std::move(*invite)};
  for (int round = 0; round < 1200 && connector.view().step != Connector::Step::Connected;
       ++round) {
    std::this_thread::sleep_for(100ms);
  }
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
  std::printf("%s\n", hex(connection->keys.key(kLinkPurpose, Role::Host)).c_str());
  return 0;
}

int run_root(const std::filesystem::path &dir) {
  const auto zerotier = node(dir, "root", kRootPort);
  while (zerotier) {
    std::this_thread::sleep_for(1s);
  }
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

Child spawn(const std::string &self, const std::vector<std::string> &arguments, bool read) {
  std::vector<std::string> words{self};
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
      posix_spawn(&child.pid, self.c_str(), &actions, nullptr, argv.data(), environ);
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

int run_test(const std::string &self) {
  const std::filesystem::path dir =
      std::filesystem::current_path() / ("zerotier-test-" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  make_root(dir);
  Child root = spawn(self, {"root", dir.string()}, false);
  Child host = spawn(self, {"host", dir.string()}, true);
  const std::string code = first_line(host.output);
  assert(code.starts_with("TST"));
  Child other = spawn(self, {"friend", dir.string(), code}, true);
  const std::string friend_key = first_line(other.output);
  const std::string host_key = first_line(host.output);
  const int friend_status = other.wait();
  const int host_status = host.wait();
  kill(root.pid, SIGTERM);
  root.wait();
  std::filesystem::remove_all(dir);
  assert(friend_status == 0 && host_status == 0);
  assert(!host_key.empty() && host_key == friend_key);
  std::puts("two processes connected over ZeroTier alone and agree on keys");
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  const std::string self = std::filesystem::canonical("/proc/self/exe").string();
  if (argc == 1) {
    return run_test(self);
  }
  const std::string role = argv[1];
  const std::filesystem::path dir = argv[2];
  if (role == "root") {
    return run_root(dir);
  }
  if (role == "host") {
    return run_host(dir);
  }
  return run_friend(dir, argv[3]);
}

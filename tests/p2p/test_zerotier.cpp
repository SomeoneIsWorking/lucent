// Two processes connect by a code over embedded ZeroTier alone, through `lucent_zerotier_root` in
// a third, and a stranger holding only the code's public half does not: one libzt node per
// `lucent_p2p_zerotier_peer`. ctest runs it in a network namespace with a dummy interface, as
// ZeroTier sends nothing over loopback. `--friend-runner wine --friend-peer <peer.exe>` seats a
// Windows build as the friend.
#include <array>
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace {

using namespace std::chrono_literals;

constexpr const char *kLabAddress = "10.77.0.1";
constexpr std::uint16_t kRootPort = 39993;

std::string read_file(const std::filesystem::path &path) {
  std::ifstream in{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{in}, {}};
}

// A child running one role, its standard output read through `output`.
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

Child spawn(std::vector<std::string> words, bool read) {
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
      posix_spawnp(&child.pid, words.front().c_str(), &actions, nullptr, argv.data(), environ);
  assert(spawned == 0);
  posix_spawn_file_actions_destroy(&actions);
  if (read) {
    close(pipe_ends[1]);
    child.output = pipe_ends[0];
  }
  return child;
}

// One line of a child's output, without the carriage return a Windows build ends it with.
std::string first_line(int output) {
  std::string text;
  char byte = 0;
  while (::read(output, &byte, 1) == 1 && byte != '\n') {
    text += byte;
  }
  if (!text.empty() && text.back() == '\r') {
    text.pop_back();
  }
  return text;
}

struct Programs {
  std::string root;
  std::string peer;
  // The friend's seat: `friend_runner` (empty for none) running `friend_peer`.
  std::string friend_runner;
  std::string friend_peer;
};

std::vector<std::string> role(const std::string &program, std::string_view name,
                              const std::filesystem::path &dir, std::uint16_t port,
                              const std::string &code) {
  std::vector<std::string> words{program, std::string{name}, dir.string(), std::to_string(port)};
  if (!code.empty()) {
    words.push_back(code);
  }
  return words;
}

int run_test(const Programs &programs) {
  const std::filesystem::path dir =
      std::filesystem::current_path() / ("zerotier-test-" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  const std::filesystem::path roots = dir / "roots";
  Child root = spawn({programs.root, (dir / "root").string(),
                      std::string{kLabAddress} + "/" + std::to_string(kRootPort), roots.string()},
                     false);
  // The root writes its root set before its node starts.
  for (int tries = 0; tries < 100 && read_file(roots).empty(); ++tries) {
    std::this_thread::sleep_for(100ms);
  }
  assert(!read_file(roots).empty());
  Child host = spawn(role(programs.peer, "host", dir, kRootPort + 1, {}), true);
  const std::string code = first_line(host.output);
  assert(code.starts_with("TST"));
  Child stranger = spawn(role(programs.peer, "stranger", dir, kRootPort + 3, code), true);
  std::vector<std::string> seat = role(programs.friend_peer, "friend", dir, kRootPort + 2, code);
  if (!programs.friend_runner.empty()) {
    seat.insert(seat.begin(), programs.friend_runner);
  }
  Child other = spawn(std::move(seat), true);
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
  std::printf("two processes connected over ZeroTier alone and agree on keys; a stranger did "
              "not%s\n",
              programs.friend_runner.empty() ? "" : " (the friend under a runner)");
  return 0;
}

} // namespace

// --root <lucent_zerotier_root> --peer <lucent_p2p_zerotier_peer>
//   [--friend-runner <program>] [--friend-peer <peer for the friend>]
int main(int argc, char **argv) {
  Programs programs;
  for (int at = 1; at + 1 < argc; at += 2) {
    const std::string_view flag = argv[at];
    const std::string value = argv[at + 1];
    if (flag == "--root") {
      programs.root = value;
    } else if (flag == "--peer") {
      programs.peer = value;
    } else if (flag == "--friend-runner") {
      programs.friend_runner = value;
    } else if (flag == "--friend-peer") {
      programs.friend_peer = value;
    } else {
      std::fprintf(stderr, "unknown flag %s\n", argv[at]);
      return 2;
    }
  }
  if (programs.root.empty() || programs.peer.empty()) {
    std::fprintf(stderr,
                 "usage: %s --root <root> --peer <peer> [--friend-runner <program>] "
                 "[--friend-peer <peer>]\n",
                 argv[0]);
    return 2;
  }
  if (programs.friend_peer.empty()) {
    programs.friend_peer = programs.peer;
  }
  return run_test(programs);
}

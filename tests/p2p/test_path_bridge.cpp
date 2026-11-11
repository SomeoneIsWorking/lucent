// A local process reaches the peer through the bridge both ways; a stranger on the path does not
// reach the process.
#include "lucent/p2p/path_bridge.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>

#include "loopback_paths.h"

namespace {

using namespace lucent::p2p;
using namespace std::chrono_literals;

std::string received(Path &path, PathAddress &from) {
  std::array<char, 256> buffer{};
  const std::optional<std::size_t> length = path.receive(buffer, from, 2s);
  return length ? std::string{buffer.data(), *length} : std::string{};
}

} // namespace

int main() {
  std::unique_ptr<UdpPath> carried = test::bound_path();
  const PathAddress carried_at = test::loopback_of(*carried);
  std::unique_ptr<UdpPath> peer = test::bound_path();
  std::unique_ptr<UdpPath> process = test::bound_path();
  std::unique_ptr<UdpPath> stranger = test::bound_path();
  std::string error;
  const std::unique_ptr<PathBridge> bridge =
      PathBridge::start(std::move(carried), test::loopback_of(*peer), error);
  assert(bridge && error.empty());
  const PathAddress bridge_at = PathAddress::from(bridge->local());

  process->send(bridge_at, "out");
  PathAddress from;
  assert(received(*peer, from) == "out");
  assert(from == carried_at);

  stranger->send(carried_at, "stranger");
  peer->send(carried_at, "back");
  assert(received(*process, from) == "back");
  assert(from == bridge_at);
  std::puts("a process reached its peer through the bridge both ways; a stranger did not");
  return 0;
}

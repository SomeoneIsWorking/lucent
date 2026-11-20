#include <lucent/line_socket.h>
#include "test_environment.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using Server = lucent::net::LineSocketServer;
using Kind = Server::Event::Kind;

int connect_to(const std::filesystem::path &path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, path.c_str(), sizeof address.sun_path - 1);
  if (::connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof address) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

// Polls until `count` events have come or a second has passed.
std::vector<Server::Event> poll_for(Server &server, std::size_t count) {
  std::vector<Server::Event> events;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{1};
  while (events.size() < count && std::chrono::steady_clock::now() < until) {
    for (Server::Event &event : server.poll()) {
      events.push_back(std::move(event));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
  }
  return events;
}

std::string read_some(int fd) {
  char buffer[256];
  const ssize_t got = ::recv(fd, buffer, sizeof buffer, 0);
  return got > 0 ? std::string(buffer, static_cast<std::size_t>(got)) : std::string{};
}

} // namespace

int main() {
  return lucent::test::run_main([] {
    int failures = 0;
    auto fail = [&failures](const char *what) {
      std::cerr << what << '\n';
      ++failures;
    };
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("lucent-ls-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / "s";
    {
      std::string error;
      std::optional<Server> server = Server::listen(path, error, {.line = 16, .peers = 2});
      if (!server) {
        std::cerr << "listen refused: " << error << '\n';
        return 1;
      }
      struct stat info{};
      if (::stat(path.c_str(), &info) != 0 ||
          (static_cast<unsigned>(info.st_mode) & 0777U) != 0600U) {
        fail("the socket is not mode 0600");
      }
      const int a = connect_to(path);
      if (a < 0) {
        std::cerr << "cannot connect to the socket\n";
        return 1;
      }
      const std::string two = "{\"hello\":1}\r\nsecond\npart";
      ::send(a, two.data(), two.size(), 0);
      std::vector<Server::Event> events = poll_for(*server, 3);
      if (events.size() != 3 || events[0].kind != Kind::Opened ||
          events[1].line != "{\"hello\":1}" || events[2].line != "second") {
        fail("a peer's lines did not arrive whole, in order");
      }
      const std::uint32_t first = events.empty() ? 0 : events[0].peer;
      ::send(a, "\n", 1, 0);
      events = poll_for(*server, 1);
      if (events.size() != 1 || events[0].line != "part") {
        fail("a line split across sends was not joined");
      }
      if (!server->send(first, "{\"screen\":1}") || read_some(a) != "{\"screen\":1}\n") {
        fail("a sent line did not reach the peer with its newline");
      }
      const int b = connect_to(path);
      const int c = connect_to(path);
      if (b < 0 || c < 0) {
        std::cerr << "cannot connect more peers\n";
        return 1;
      }
      events = poll_for(*server, 1);
      if (server->peers().size() != 2) {
        fail("a peer past the limit was kept");
      }
      const std::string longer(40, 'x');
      ::send(b, longer.data(), longer.size(), 0);
      events = poll_for(*server, 1);
      if (events.empty() || events.back().kind != Kind::Closed) {
        fail("a peer sending a line past the limit was kept");
      }
      ::close(a);
      events = poll_for(*server, 1);
      if (events.empty() || events.back().kind != Kind::Closed || events.back().peer != first) {
        fail("a peer hanging up was not told");
      }
      if (server->send(first, "gone")) {
        fail("a send to a closed peer succeeded");
      }
      ::close(b);
      ::close(c);
    }
    if (std::filesystem::exists(path)) {
      fail("the socket's file outlived the server");
    }
    std::string error;
    std::filesystem::create_directories(path);
    if (Server::listen(path, error)) {
      fail("listen replaced something that is not a socket");
    }
    std::filesystem::remove_all(dir);
    return failures;
  });
}

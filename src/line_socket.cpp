#include "lucent/line_socket.h"

#include <utility>

#if defined(_WIN32)

namespace lucent::net {

struct LineSocketServer::State {
  std::filesystem::path path;
};

std::optional<LineSocketServer> LineSocketServer::listen(const std::filesystem::path &,
                                                         std::string &error, Limits) {
  error = "Unix line sockets are not available on this system";
  return std::nullopt;
}

LineSocketServer::LineSocketServer(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {
}
LineSocketServer::~LineSocketServer() = default;
LineSocketServer::LineSocketServer(LineSocketServer &&) noexcept = default;
LineSocketServer &LineSocketServer::operator=(LineSocketServer &&) noexcept = default;
const std::filesystem::path &LineSocketServer::path() const noexcept {
  return state_->path;
}
std::vector<LineSocketServer::Event> LineSocketServer::poll() {
  return {};
}
bool LineSocketServer::send(std::uint32_t, std::string_view) {
  return false;
}
void LineSocketServer::send_all(std::string_view) {
}
std::vector<std::uint32_t> LineSocketServer::peers() const {
  return {};
}

} // namespace lucent::net

#else

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace lucent::net {
namespace {

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

bool non_blocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  return flags >= 0 &&
         fcntl(fd, F_SETFL, static_cast<unsigned>(flags) | static_cast<unsigned>(O_NONBLOCK)) ==
             0 &&
         fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

void no_sigpipe([[maybe_unused]] int fd) {
#if defined(SO_NOSIGPIPE)
  const int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
}

std::string system_error(const char *what) {
  return std::string{what} + ": " + std::strerror(errno);
}

} // namespace

struct LineSocketServer::State {
  struct Peer {
    std::uint32_t id = 0;
    int fd = -1;
    std::string in;
    std::string out;
    bool closing = false;
  };

  std::filesystem::path path;
  Limits limits;
  int listener = -1;
  std::uint32_t next = 1;
  std::vector<Peer> peers;

  ~State() {
    for (Peer &peer : peers) {
      ::close(peer.fd);
    }
    if (listener >= 0) {
      ::close(listener);
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  }

  // Writes what the socket takes now; false when the peer is gone or too far behind.
  bool flush(Peer &peer) {
    while (!peer.out.empty()) {
      const ssize_t sent = ::send(peer.fd, peer.out.data(), peer.out.size(), kSendFlags);
      if (sent > 0) {
        peer.out.erase(0, static_cast<std::size_t>(sent));
        continue;
      }
      if (sent < 0 && errno == EINTR) {
        continue;
      }
      if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return peer.out.size() <= limits.backlog;
      }
      return false;
    }
    return true;
  }

  // Reads what has arrived into lines; false when the peer hung up or broke the bounds.
  bool read(Peer &peer, std::vector<Event> &events) {
    char buffer[16 * 1024];
    for (;;) {
      const ssize_t got = ::recv(peer.fd, buffer, sizeof buffer, 0);
      if (got > 0) {
        peer.in.append(buffer, static_cast<std::size_t>(got));
        std::size_t start = 0;
        for (std::size_t end = peer.in.find('\n'); end != std::string::npos;
             end = peer.in.find('\n', start)) {
          std::string line = peer.in.substr(start, end - start);
          if (!line.empty() && line.back() == '\r') {
            line.pop_back();
          }
          if (line.size() > limits.line) {
            return false;
          }
          events.push_back({Event::Kind::Line, peer.id, std::move(line)});
          start = end + 1;
        }
        peer.in.erase(0, start);
        if (peer.in.size() > limits.line) {
          return false;
        }
        continue;
      }
      if (got < 0 && errno == EINTR) {
        continue;
      }
      return got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
    }
  }
};

std::optional<LineSocketServer> LineSocketServer::listen(const std::filesystem::path &path,
                                                         std::string &error, Limits limits) {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const std::string native = path.string();
  if (native.empty() || native.size() >= sizeof address.sun_path) {
    error = "the socket path is " + std::to_string(native.size()) + " bytes, past the " +
            std::to_string(sizeof address.sun_path - 1) + " a socket address holds";
    return std::nullopt;
  }
  std::memcpy(address.sun_path, native.c_str(), native.size() + 1);

  struct stat existing{};
  if (::lstat(native.c_str(), &existing) == 0) {
    if (!S_ISSOCK(existing.st_mode)) {
      error = native + " exists and is not a socket";
      return std::nullopt;
    }
    ::unlink(native.c_str());
  }

  auto state = std::make_unique<State>();
  state->limits = limits;
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    error = system_error("socket");
    return std::nullopt;
  }
  // The file is made by bind under the umask; one that keeps everyone else out from the start
  // leaves no moment another user could connect.
  const mode_t old_mask = ::umask(0077);
  const int bound = ::bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof address);
  ::umask(old_mask);
  if (bound != 0) {
    error = system_error("bind");
    ::close(fd);
    return std::nullopt;
  }
  state->listener = fd;
  state->path = path;
  if (::chmod(native.c_str(), 0600) != 0 || ::listen(fd, 8) != 0 || !non_blocking(fd)) {
    error = system_error("listen");
    return std::nullopt;
  }
  return LineSocketServer{std::move(state)};
}

LineSocketServer::LineSocketServer(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {
}
LineSocketServer::~LineSocketServer() = default;
LineSocketServer::LineSocketServer(LineSocketServer &&) noexcept = default;
LineSocketServer &LineSocketServer::operator=(LineSocketServer &&) noexcept = default;

const std::filesystem::path &LineSocketServer::path() const noexcept {
  return state_->path;
}

std::vector<LineSocketServer::Event> LineSocketServer::poll() {
  std::vector<Event> events;
  for (;;) {
    const int fd = ::accept(state_->listener, nullptr, nullptr);
    if (fd < 0) {
      break;
    }
    if (state_->peers.size() >= state_->limits.peers || !non_blocking(fd)) {
      ::close(fd);
      continue;
    }
    no_sigpipe(fd);
    const std::uint32_t id = state_->next++;
    state_->peers.push_back({.id = id, .fd = fd, .in = {}, .out = {}, .closing = false});
    events.push_back({Event::Kind::Opened, id, {}});
  }
  for (State::Peer &peer : state_->peers) {
    if (!peer.closing && (!state_->read(peer, events) || !state_->flush(peer))) {
      peer.closing = true;
    }
  }
  for (auto it = state_->peers.begin(); it != state_->peers.end();) {
    if (it->closing) {
      ::close(it->fd);
      events.push_back({Event::Kind::Closed, it->id, {}});
      it = state_->peers.erase(it);
    } else {
      ++it;
    }
  }
  return events;
}

bool LineSocketServer::send(std::uint32_t peer, std::string_view line) {
  for (State::Peer &each : state_->peers) {
    if (each.id != peer || each.closing) {
      continue;
    }
    each.out.append(line);
    each.out.push_back('\n');
    if (!state_->flush(each)) {
      each.closing = true;
      return false;
    }
    return true;
  }
  return false;
}

void LineSocketServer::send_all(std::string_view line) {
  for (const std::uint32_t peer : peers()) {
    send(peer, line);
  }
}

std::vector<std::uint32_t> LineSocketServer::peers() const {
  std::vector<std::uint32_t> ids;
  for (const State::Peer &peer : state_->peers) {
    if (!peer.closing) {
      ids.push_back(peer.id);
    }
  }
  return ids;
}

} // namespace lucent::net

#endif

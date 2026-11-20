// lucent/line_socket.h — a local channel of newline-delimited messages (JSON lines, say) on a Unix
// socket, read by polling from the owner's loop. Lucent owns the socket, its permissions, framing
// and bounds; the consumer owns what the lines say.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lucent::net {

// Listens on one Unix socket that only its user can open. Never blocks: `poll` accepts and reads
// what has arrived, `send` writes what the peer takes now and keeps the rest for later calls. POSIX
// only; elsewhere `listen` refuses.
class LineSocketServer {
public:
  struct Limits {
    // The longest line a peer may send, without its newline; a longer one closes that peer.
    std::size_t line = std::size_t{64} * 1024;
    // Peers at once; one more is closed as it connects.
    std::size_t peers = 16;
    // Bytes waiting for a peer that does not read; past it the peer is closed.
    std::size_t backlog = std::size_t{1024} * 1024;
  };

  struct Event {
    enum class Kind : std::uint8_t { Opened, Line, Closed };
    Kind kind = Kind::Line;
    // Numbers peers from 1, never reused while the server lives.
    std::uint32_t peer = 0;
    // A Line's text, without its newline (or a trailing CR).
    std::string line;
  };

  // Listens at `path` (mode 0600), replacing a socket left there; nothing, with `error`, when the
  // path is too long for a socket address, holds something else, or the system refuses.
  static std::optional<LineSocketServer> listen(const std::filesystem::path &path,
                                                std::string &error, Limits limits);
  static std::optional<LineSocketServer> listen(const std::filesystem::path &path,
                                                std::string &error) {
    return listen(path, error, Limits{});
  }

  // Closes every peer and removes the socket's file.
  ~LineSocketServer();
  LineSocketServer(LineSocketServer &&other) noexcept;
  LineSocketServer &operator=(LineSocketServer &&other) noexcept;
  LineSocketServer(const LineSocketServer &) = delete;
  LineSocketServer &operator=(const LineSocketServer &) = delete;

  const std::filesystem::path &path() const noexcept;
  // Peers that connected, lines that arrived and peers that left since the last call, in order.
  std::vector<Event> poll();
  // Sends `line` and a newline to `peer`; false when it is not connected (or was just closed for
  // falling behind).
  bool send(std::uint32_t peer, std::string_view line);
  // Sends `line` to every connected peer.
  void send_all(std::string_view line);
  // The peers connected now.
  std::vector<std::uint32_t> peers() const;

private:
  struct State;
  explicit LineSocketServer(std::unique_ptr<State> state) noexcept;
  std::unique_ptr<State> state_;
};

} // namespace lucent::net

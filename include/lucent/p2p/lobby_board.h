#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "lucent/http.h"
#include "lucent/p2p/directory.h"

namespace lucent::p2p {

// A lobby lapses this long after its host last published it.
inline constexpr std::int64_t kBoardLobbySeconds = 300;
inline constexpr std::size_t kMaxBoardLobbiesPerGame = 500;
inline constexpr std::size_t kMaxBoardGames = 256;

// The self-hosted lobby list, as the handler of a `lucent::http::Server`:
//   PUT  /v1/lobbies           {"game","token","lobby":{...}} publishes or replaces
//   POST /v1/lobbies/withdraw  {"game","token","id"}          takes it off
//   GET  /v1/lobbies?game=G                                    {"lobbies":[...]}, newest first
// A host is the SHA-256 of its token, so only the token's holder replaces or withdraws its lobby.
class LobbyBoard {
public:
  using Clock = std::function<std::int64_t()>;

  // `now` gives Unix seconds; the system clock unless a test steps one.
  explicit LobbyBoard(Clock now = {});

  http::Response handle(const http::Request &request);

private:
  struct Entry {
    Lobby lobby;
    std::int64_t published_at = 0;
  };
  // Host, then lobby id.
  using Lobbies = std::map<std::pair<std::string, std::string>, Entry>;

  http::Response publish(const http::Request &request);
  http::Response withdraw(const http::Request &request);
  http::Response list(const http::Request &request);
  void prune(std::int64_t now);

  Clock now_;
  std::mutex mutex_;
  std::map<std::string, Lobbies> games_;
};

} // namespace lucent::p2p

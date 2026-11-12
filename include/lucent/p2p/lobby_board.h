#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "lucent/http.h"
#include "lucent/p2p/directory.h"

namespace lucent::p2p {

inline constexpr std::size_t kMaxBoardLobbiesPerGame = 500;
inline constexpr std::size_t kMaxBoardGames = 256;
inline constexpr std::size_t kMaxBoardMailboxesPerGame = 4096;
// Letters one mailbox takes a minute; more are refused with 429.
inline constexpr std::size_t kMaxBoardLettersPerMinute = 16;

// The self-hosted lobby list, as the handler of a `lucent::http::Server`:
//   PUT  /v1/lobbies           {"game","token","lobby":{...}} publishes or replaces
//   POST /v1/lobbies/withdraw  {"game","token","id"}          takes it off
//   GET  /v1/lobbies?game=G                                    {"lobbies":[...]}, newest first
//   POST /v1/letters           {"game","mailbox","letter"}      leaves a letter for kLetterSeconds
//   GET  /v1/letters?game=G&mailbox=M                          {"letters":[...]}, newest first
// A host is the SHA-256 of its token, so only the token's holder replaces or withdraws its lobby.
// Letters are sealed, so the board neither reads nor needs to guard them beyond their size and
// rate.
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
  struct Letter {
    std::string text;
    std::int64_t posted_at = 0;
  };
  // Mailbox to its letters, oldest first.
  using Mailboxes = std::map<std::string, std::deque<Letter>>;

  http::Response publish(const http::Request &request);
  http::Response withdraw(const http::Request &request);
  http::Response list(const http::Request &request);
  http::Response post(const http::Request &request);
  http::Response letters(const http::Request &request);
  void prune(std::int64_t now);

  Clock now_;
  std::mutex mutex_;
  std::map<std::string, Lobbies> games_;
  std::map<std::string, Mailboxes> mail_;
};

} // namespace lucent::p2p

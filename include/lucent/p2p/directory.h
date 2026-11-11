#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lucent/json.h"

// Where a game lists its open lobbies, so players find one without trading a code by hand. A
// directory only carries the invite code a host would share; every game stays playable by code
// with each directory off or down.
namespace lucent::p2p {

struct Lobby {
  // Unique among this host's lobbies.
  std::string id;
  std::string title;
  // The connector's invite code.
  std::string invite;
  int players = 0;
  int capacity = 0;
  // The game build, so a list can leave out hosts it cannot play with.
  std::string version;
  // Filled in by `list`: who published it, and when it lapses (Unix seconds).
  std::string host;
  std::int64_t expires_at = 0;
};

// Longest fields a directory carries; a lobby past one is not published or listed.
inline constexpr std::size_t kMaxLobbyIdBytes = 64;
inline constexpr std::size_t kMaxLobbyTitleBytes = 128;
inline constexpr std::size_t kMaxLobbyInviteBytes = 2048;
inline constexpr std::size_t kMaxLobbyVersionBytes = 64;
inline constexpr int kMaxLobbyCapacity = 1024;
// A lobby lapses this long after its host last published it, in every directory.
inline constexpr std::int64_t kLobbySeconds = 300;

// The system clock in Unix seconds, the unit of `Lobby::expires_at`.
std::int64_t unix_seconds();

// A game's name in a directory: 1 to 32 of a-z, 0-9, '-', '_' and '.'.
bool valid_game_name(std::string_view game) noexcept;
// The lobby's id, title, invite, players, capacity and version as a JSON object, plus its host
// and expiry when `listing`.
std::string lobby_json(const Lobby &lobby, bool listing);
// Within every limit above, with an id, an invite, and seats that add up.
bool publishable(const Lobby &lobby) noexcept;
// The lobby a JSON object describes, host and expiry included when present; nothing for a missing
// or mistyped field or one that is not publishable.
std::optional<Lobby> lobby_of_json(const json::Value &value);

class Directory {
public:
  Directory() = default;
  Directory(const Directory &) = delete;
  Directory &operator=(const Directory &) = delete;
  Directory(Directory &&) = delete;
  Directory &operator=(Directory &&) = delete;
  virtual ~Directory() = default;

  // Lists or replaces this host's lobby; it lapses unless published again within the
  // directory's lifetime. False, with `error`, when no part of the directory took it.
  virtual bool publish(const Lobby &lobby, std::string &error) = 0;
  // The open lobbies of this game, newest first; nothing when no part of the directory answered.
  virtual std::optional<std::vector<Lobby>> list(std::string &error) = 0;
  // Takes this host's lobby `id` off the list.
  virtual bool withdraw(const std::string &id, std::string &error) = 0;
};

} // namespace lucent::p2p

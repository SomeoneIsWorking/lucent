#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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

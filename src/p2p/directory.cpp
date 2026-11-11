#include "lucent/p2p/directory.h"

#include "lucent/text.h"

#include <algorithm>
#include <chrono>

namespace lucent::p2p {
namespace {

constexpr std::size_t kMaxGameNameBytes = 32;

const std::string *text_member(const json::Value &value, std::string_view key) {
  const json::Value *member = value.find(key);
  return member != nullptr ? member->string() : nullptr;
}

std::optional<std::int64_t> integer_member(const json::Value &value, std::string_view key) {
  const json::Value *member = value.find(key);
  return member != nullptr ? member->integer() : std::nullopt;
}

} // namespace

std::int64_t unix_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool valid_game_name(std::string_view game) noexcept {
  if (game.empty() || game.size() > kMaxGameNameBytes) {
    return false;
  }
  for (const char c : game) {
    const bool allowed =
        (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    if (!allowed) {
      return false;
    }
  }
  return true;
}

std::string lobby_json(const Lobby &lobby, bool listing) {
  std::string out = "{\"id\":" + text::json_quote(lobby.id) +
                    ",\"title\":" + text::json_quote(lobby.title) +
                    ",\"invite\":" + text::json_quote(lobby.invite) +
                    ",\"players\":" + std::to_string(lobby.players) +
                    ",\"capacity\":" + std::to_string(lobby.capacity) +
                    ",\"version\":" + text::json_quote(lobby.version);
  if (listing) {
    out += ",\"host\":" + text::json_quote(lobby.host) +
           ",\"expires_at\":" + std::to_string(lobby.expires_at);
  }
  out.push_back('}');
  return out;
}

bool publishable(const Lobby &lobby) noexcept {
  return !lobby.id.empty() && lobby.id.size() <= kMaxLobbyIdBytes &&
         lobby.title.size() <= kMaxLobbyTitleBytes && !lobby.invite.empty() &&
         lobby.invite.size() <= kMaxLobbyInviteBytes &&
         lobby.version.size() <= kMaxLobbyVersionBytes && lobby.players >= 0 &&
         lobby.capacity >= lobby.players && lobby.capacity <= kMaxLobbyCapacity;
}

std::optional<Lobby> lobby_of_json(const json::Value &value) {
  const std::string *id = text_member(value, "id");
  const std::string *title = text_member(value, "title");
  const std::string *invite = text_member(value, "invite");
  const std::string *version = text_member(value, "version");
  const std::optional<std::int64_t> players = integer_member(value, "players");
  const std::optional<std::int64_t> capacity = integer_member(value, "capacity");
  if (id == nullptr || title == nullptr || invite == nullptr || version == nullptr || !players ||
      !capacity || *players < 0 || *capacity > kMaxLobbyCapacity) {
    return std::nullopt;
  }
  Lobby lobby;
  lobby.id = *id;
  lobby.title = *title;
  lobby.invite = *invite;
  lobby.players = static_cast<int>(std::min<std::int64_t>(*players, kMaxLobbyCapacity + 1));
  lobby.capacity = static_cast<int>(*capacity);
  lobby.version = *version;
  if (const std::string *host = text_member(value, "host")) {
    lobby.host = *host;
  }
  if (const std::optional<std::int64_t> expires = integer_member(value, "expires_at")) {
    lobby.expires_at = *expires;
  }
  if (!publishable(lobby)) {
    return std::nullopt;
  }
  return lobby;
}

} // namespace lucent::p2p

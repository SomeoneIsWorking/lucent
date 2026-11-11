#include "lucent/p2p/lobby_board.h"

#include "lucent/json.h"
#include "lucent/p2p/crypto.h"
#include "lucent/text.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace lucent::p2p {
namespace {

// A token is 32 random bytes as hex.
constexpr std::size_t kTokenHexDigits = 64;

std::int64_t system_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

http::Response refused(int status, std::string reason, std::string_view why) {
  return http::Response::json(status, std::move(reason),
                              "{\"error\":" + text::json_quote(why) + "}");
}

struct Signed {
  std::string game;
  std::string host;
  json::Value body;
};

// The game and the token's host from a request body, or why it is refused.
std::optional<Signed> signed_body(const http::Request &request, std::string &why) {
  std::string error;
  std::optional<json::Value> body = json::parse(request.body, error);
  const json::Value *game = body ? body->find("game") : nullptr;
  const json::Value *token = body ? body->find("token") : nullptr;
  if (!body || game == nullptr || game->string() == nullptr || !valid_game_name(*game->string()) ||
      token == nullptr || token->string() == nullptr ||
      token->string()->size() != kTokenHexDigits) {
    why = "a body with a valid game and a 64-digit token is needed";
    return std::nullopt;
  }
  const std::optional<std::vector<std::uint8_t>> secret = text::from_hex(*token->string());
  if (!secret) {
    why = "the token is not hex";
    return std::nullopt;
  }
  return Signed{*game->string(), text::hex(crypto::sha256(*secret)), std::move(*body)};
}

} // namespace

LobbyBoard::LobbyBoard(Clock now) : now_{now ? std::move(now) : Clock{system_seconds}} {
}

http::Response LobbyBoard::handle(const http::Request &request) {
  const std::string_view path = request.path();
  if (path == "/v1/lobbies" && request.method == "PUT") {
    return publish(request);
  }
  if (path == "/v1/lobbies/withdraw" && request.method == "POST") {
    return withdraw(request);
  }
  if (path == "/v1/lobbies" && request.method == "GET") {
    return list(request);
  }
  if (path == "/v1/lobbies" || path == "/v1/lobbies/withdraw") {
    return refused(405, "Method Not Allowed", "not a method this path takes");
  }
  return refused(404, "Not Found", "no such path");
}

void LobbyBoard::prune(std::int64_t now) {
  for (auto game = games_.begin(); game != games_.end();) {
    std::erase_if(game->second, [now](const auto &entry) {
      return entry.second.lobby.expires_at <= now;
    });
    game = game->second.empty() ? games_.erase(game) : std::next(game);
  }
}

http::Response LobbyBoard::publish(const http::Request &request) {
  std::string why;
  std::optional<Signed> body = signed_body(request, why);
  if (!body) {
    return refused(400, "Bad Request", why);
  }
  const json::Value *offered = body->body.find("lobby");
  std::optional<Lobby> lobby = offered != nullptr ? lobby_of_json(*offered) : std::nullopt;
  if (!lobby) {
    return refused(400, "Bad Request", "the lobby is not publishable");
  }
  const std::int64_t now = now_();
  lobby->host = body->host;
  lobby->expires_at = now + kBoardLobbySeconds;
  const std::lock_guard lock{mutex_};
  prune(now);
  if (!games_.contains(body->game) && games_.size() >= kMaxBoardGames) {
    return refused(503, "Service Unavailable", "the board holds as many games as it takes");
  }
  Lobbies &lobbies = games_[body->game];
  const std::pair key{lobby->host, lobby->id};
  if (!lobbies.contains(key) && lobbies.size() >= kMaxBoardLobbiesPerGame) {
    return refused(503, "Service Unavailable", "the game holds as many lobbies as it takes");
  }
  const std::int64_t expires = lobby->expires_at;
  lobbies[key] = Entry{std::move(*lobby), now};
  return http::Response::json(200, "OK", "{\"expires_at\":" + std::to_string(expires) + "}");
}

http::Response LobbyBoard::withdraw(const http::Request &request) {
  std::string why;
  const std::optional<Signed> body = signed_body(request, why);
  if (!body) {
    return refused(400, "Bad Request", why);
  }
  const json::Value *id = body->body.find("id");
  if (id == nullptr || id->string() == nullptr) {
    return refused(400, "Bad Request", "an id is needed");
  }
  const std::lock_guard lock{mutex_};
  const auto game = games_.find(body->game);
  if (game != games_.end()) {
    game->second.erase({body->host, *id->string()});
  }
  return http::Response::json(200, "OK", "{}");
}

http::Response LobbyBoard::list(const http::Request &request) {
  std::vector<http::FormField> fields;
  std::string error;
  std::string game;
  if (http::parse_form_urlencoded(request.query(), fields, error)) {
    for (const http::FormField &field : fields) {
      if (field.name == "game") {
        game = field.value;
      }
    }
  }
  if (!valid_game_name(game)) {
    return refused(400, "Bad Request", "a valid game is needed");
  }
  std::vector<const Entry *> open;
  std::string out = "{\"lobbies\":[";
  const std::lock_guard lock{mutex_};
  prune(now_());
  if (const auto found = games_.find(game); found != games_.end()) {
    for (const auto &[key, entry] : found->second) {
      open.push_back(&entry);
    }
  }
  std::stable_sort(open.begin(), open.end(), [](const Entry *first, const Entry *second) {
    return first->published_at > second->published_at;
  });
  for (std::size_t index = 0; index < open.size(); ++index) {
    out += (index == 0 ? "" : ",") + lobby_json(open[index]->lobby, true);
  }
  out += "]}";
  return http::Response::json(200, "OK", std::move(out));
}

} // namespace lucent::p2p

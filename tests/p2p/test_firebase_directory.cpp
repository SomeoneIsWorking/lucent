// Firebase directories against a loopback stand-in of the auth and database REST APIs, or with
// `--emulator <database> <auth> <namespace>` against the Firebase Local Emulator Suite running
// `firebase_rules_json()`: a lobby is listed with its fields, only its own player writes or
// withdraws it, an expired one is not listed and is swept by the next publish, and an ID token is
// refreshed for the same user. `--rules` prints the database rules.
#include "lucent/p2p/firebase_directory.h"

#include "lucent/http.h"
#include "lucent/http_client.h"
#include "lucent/json.h"
#include "lucent/p2p/tls_stream.h"
#include "lucent/text.h"
#include "lucent/url.h"

#include <cassert>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using lucent::http::Request;
using lucent::http::Response;

std::optional<std::string> query_value(std::string_view query, const char *name) {
  std::vector<lucent::http::FormField> fields;
  std::string error;
  if (!lucent::http::parse_form_urlencoded(query, fields, error)) {
    return std::nullopt;
  }
  for (const lucent::http::FormField &field : fields) {
    if (field.name == name) {
      return field.value;
    }
  }
  return std::nullopt;
}

// A stored lobby's expiry, 0 when it does not read.
std::int64_t expiry_of(const std::string &text) {
  std::string error;
  const std::optional<lucent::json::Value> value = lucent::json::parse(text, error);
  const lucent::json::Value *expires = value ? value->find("expires_at") : nullptr;
  return expires != nullptr ? expires->integer().value_or(0) : 0;
}

std::optional<std::int64_t> number(std::string_view digits) {
  std::string error;
  const std::optional<lucent::json::Value> value = lucent::json::parse(digits, error);
  return value ? value->integer() : std::nullopt;
}

// Google's two auth services and one database, as far as the directory uses them.
class StandIn {
public:
  Response handle(const Request &request) {
    const std::lock_guard lock{mutex_};
    const std::string_view path = request.path();
    const std::string_view query = request.query();
    if (query_value(query, "key").value_or("") != "") {
      if (path == "/identitytoolkit.googleapis.com/v1/accounts:signUp") {
        const std::string user = "user" + std::to_string(++users_);
        const std::string token = issue(user);
        // A short first token, so the next request refreshes it.
        return Response::json(200, "OK",
                              R"({"idToken":")" + token + R"(","refreshToken":"r-)" + user +
                                  R"(","expiresIn":"30","localId":")" + user + R"("})");
      }
      if (path == "/securetoken.googleapis.com/v1/token") {
        const std::optional<std::string> refresh = query_value(request.body, "refresh_token");
        if (query_value(request.body, "grant_type") != "refresh_token" || !refresh ||
            !refresh->starts_with("r-")) {
          return Response::json(400, "Bad Request", R"({"error":{"message":"INVALID"}})");
        }
        const std::string user = refresh->substr(2);
        ++refreshes_;
        return Response::json(200, "OK",
                              R"({"id_token":")" + issue(user) + R"(","refresh_token":")" +
                                  *refresh + R"(","expires_in":"3600","user_id":")" + user +
                                  R"("})");
      }
    }
    return database(request);
  }

  int refreshes() const {
    const std::lock_guard lock{mutex_};
    return refreshes_;
  }

private:
  std::string issue(const std::string &user) {
    const std::string token = "t" + std::to_string(++tokens_) + "-" + user;
    users_of_[token] = user;
    return token;
  }

  Response database(const Request &request) {
    const std::string_view path = request.path();
    const std::string_view query = request.query();
    constexpr std::string_view root = "/lucent/lobbies/";
    const auto token = users_of_.find(query_value(query, "auth").value_or(""));
    if (!path.starts_with(root) || !path.ends_with(".json") || token == users_of_.end()) {
      return Response::json(401, "Unauthorized", R"({"error":"Permission denied"})");
    }
    const std::string_view rest = path.substr(root.size(), path.size() - root.size() - 5);
    const std::size_t slash = rest.find('/');
    std::map<std::string, std::string> &game = lobbies_[std::string{rest.substr(0, slash)}];
    const std::int64_t now = lucent::p2p::unix_seconds();
    if (slash == std::string_view::npos) {
      if (request.method != "GET" || query_value(query, "orderBy") != "\"expires_at\"") {
        return Response::json(400, "Bad Request", R"({"error":"Index not defined"})");
      }
      const std::int64_t from = number(query_value(query, "startAt").value_or("")).value_or(0);
      const std::int64_t until =
          number(query_value(query, "endAt").value_or("")).value_or(INT64_MAX);
      std::string out;
      for (const auto &[key, text] : game) {
        const std::int64_t expires = expiry_of(text);
        if (expires >= from && expires <= until) {
          out += (out.empty() ? "{" : ",") + lucent::text::json_quote(key) + ":" + text;
        }
      }
      return Response::json(200, "OK", out.empty() ? "null" : out + "}");
    }
    const std::string key{rest.substr(slash + 1)};
    const bool own = key.starts_with(token->second + "-");
    const auto stored = game.find(key);
    if (request.method == "GET") {
      return Response::json(200, "OK", stored != game.end() ? stored->second : "null");
    }
    if (request.method == "DELETE") {
      const bool expired = stored != game.end() && expiry_of(stored->second) < now;
      if (!own && !expired && stored != game.end()) {
        return Response::json(401, "Unauthorized", R"({"error":"Permission denied"})");
      }
      game.erase(key);
      return Response::json(200, "OK", "null");
    }
    std::string error;
    const std::optional<lucent::json::Value> body = lucent::json::parse(request.body, error);
    const lucent::json::Value *host = body ? body->find("host") : nullptr;
    if (request.method != "PUT" || !own || host == nullptr || host->string() == nullptr ||
        *host->string() != token->second) {
      return Response::json(401, "Unauthorized", R"({"error":"Permission denied"})");
    }
    game[key] = request.body;
    return Response::json(200, "OK", request.body);
  }

  mutable std::mutex mutex_;
  int users_ = 0;
  int tokens_ = 0;
  int refreshes_ = 0;
  std::map<std::string, std::string> users_of_;
  std::map<std::string, std::map<std::string, std::string>> lobbies_;
};

struct Target {
  lucent::p2p::FirebaseProject project;
  std::string auth_base;
};

lucent::http::FetchResult raw(const std::string &url, std::string_view method,
                              std::optional<std::string> body) {
  const std::optional<lucent::net::Url> parsed = lucent::net::parse_url(url);
  assert(parsed);
  std::string error;
  const auto stream = lucent::p2p::connect_stream(*parsed, {}, error);
  assert(stream);
  lucent::http::FetchRequest request;
  request.method = method;
  request.target = parsed->target;
  request.headers = "Content-Type: application/json\r\n";
  if (body) {
    request.body = *body;
  }
  lucent::http::FetchResult result;
  const bool fetched = lucent::http::fetch(*stream, parsed->authority(), request, result, error);
  assert(fetched);
  return result;
}

// A player outside any directory: its user id and ID token.
std::pair<std::string, std::string> sign_up(const Target &target) {
  const auto result = raw(target.auth_base +
                              "/identitytoolkit.googleapis.com/v1/"
                              "accounts:signUp?key=" +
                              target.project.api_key,
                          "POST", std::string{R"({"returnSecureToken":true})"});
  std::string error;
  const auto reply = lucent::json::parse(result.body, error);
  assert(result.status == 200 && reply);
  return {*reply->find("localId")->string(), *reply->find("idToken")->string()};
}

std::string lobby_url(const Target &target, const std::string &key, const std::string &token) {
  std::string url = target.project.database + "/lucent/lobbies/test/" + key +
                    ".json?auth=" + lucent::text::url_encode(token);
  if (target.project.emulator_namespace) {
    url += "&ns=" + *target.project.emulator_namespace;
  }
  return url;
}

int listed_count(lucent::p2p::Directory &directory) {
  std::string error;
  const auto lobbies = directory.list(error);
  if (!lobbies) {
    std::fprintf(stderr, "list failed: %s\n", error.c_str());
  }
  return lobbies ? static_cast<int>(lobbies->size()) : -1;
}

void run(const Target &target) {
  lucent::p2p::FirebaseDirectory host{"test", target.project, {}};
  lucent::p2p::FirebaseDirectory friend_{"test", target.project, {}};
  lucent::p2p::FirebaseDirectory other_game{"other", target.project, {}};
  const lucent::p2p::Lobby lobby{"room/1", "Bar\xC3\xA9's room", "INVITE", 1, 4, "2.1", {}, 0};
  std::string error;
  const bool published = host.publish(lobby, error);
  if (!published) {
    std::fprintf(stderr, "publish failed: %s\n", error.c_str());
  }
  assert(published);
  const auto listed = friend_.list(error);
  assert(listed && listed->size() == 1);
  const lucent::p2p::Lobby found = listed.value_or(std::vector<lucent::p2p::Lobby>{{}}).front();
  const std::int64_t now = lucent::p2p::unix_seconds();
  assert(found.id == "room/1" && found.title == lobby.title && found.invite == "INVITE" &&
         found.players == 1 && found.capacity == 4 && found.version == "2.1" &&
         found.host == host.user() && found.expires_at > now &&
         found.expires_at <= now + lucent::p2p::kLobbySeconds);
  assert(listed_count(other_game) == 0);

  // Another player cannot take it down or write over it.
  const bool not_theirs = friend_.withdraw("room/1", error);
  assert(not_theirs && listed_count(friend_) == 1);
  const auto [intruder, intruder_token] = sign_up(target);
  std::string forged = lucent::p2p::lobby_json(found, true);
  const std::string host_key = host.user() + "-" + "00000000000000000000000000000000";
  assert(raw(lobby_url(target, host_key, intruder_token), "PUT", forged).status != 200);

  const bool withdrawn = host.withdraw("room/1", error);
  assert(withdrawn && listed_count(friend_) == 0);

  // An expired lobby is not listed, and the next publish sweeps it.
  lucent::p2p::Lobby stale = found;
  stale.host = intruder;
  stale.expires_at = now - 10;
  const std::string stale_key = intruder + "-stale";
  const auto put = raw(lobby_url(target, stale_key, intruder_token), "PUT",
                       lucent::p2p::lobby_json(stale, true));
  assert(put.status == 200);
  assert(listed_count(friend_) == 0);
  assert(host.publish(lobby, error));
  assert(raw(lobby_url(target, stale_key, intruder_token), "GET", std::nullopt).body == "null");
  assert(listed_count(friend_) == 1);
  assert(host.withdraw("room/1", error));

  bool thrown = false;
  try {
    lucent::p2p::FirebaseDirectory bad{"Bad Name", target.project, {}};
  } catch (const std::invalid_argument &) {
    thrown = true;
  }
  assert(thrown);
}

} // namespace

int main(int argc, char **argv) {
  std::string error;
  assert(lucent::json::parse(lucent::p2p::firebase_rules_json(), error));
  // The rules for the emulator or the console's Rules tab.
  if (argc == 2 && std::string_view{argv[1]} == "--rules") {
    std::puts(lucent::p2p::firebase_rules_json().c_str());
    return 0;
  }
  if (argc == 5 && std::string_view{argv[1]} == "--emulator") {
    Target target;
    target.project.api_key = "lucent-test";
    target.project.database = argv[2];
    target.project.auth_emulator = argv[3];
    target.project.emulator_namespace = argv[4];
    target.auth_base = argv[3];
    run(target);
    std::puts("the Firebase emulator lists, guards, withdraws and sweeps lobbies");
    return 0;
  }
  StandIn stand_in;
  lucent::http::Server server{lucent::http::ServerOptions{}, [&stand_in](const Request &request) {
                                return stand_in.handle(request);
                              }};
  const bool started = server.start();
  assert(started);
  const std::string base = "http://127.0.0.1:" + std::to_string(server.port());
  Target target;
  target.project.api_key = "lucent-test";
  target.project.database = base;
  target.project.auth_emulator = base;
  target.project.emulator_namespace = "lucent-test";
  target.auth_base = base;
  run(target);
  // Each directory's first token is short, so its next request refreshed it for the same user.
  assert(stand_in.refreshes() >= 2);
  std::puts(
      "Firebase directories list, guard, withdraw and sweep lobbies, refreshing their tokens");
  return 0;
}

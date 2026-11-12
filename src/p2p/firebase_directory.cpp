#include "lucent/p2p/firebase_directory.h"

#include "lucent/http_client.h"
#include "lucent/json.h"
#include "lucent/log.h"
#include "lucent/p2p/crypto.h"
#include "lucent/p2p/letter.h"
#include "lucent/text.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace lucent::p2p {
namespace {

constexpr std::string_view kRoot = "/lucent/";
constexpr std::size_t kMaxReplyBytes = std::size_t{2} * 1024 * 1024;
constexpr int kMaxListedLobbies = 200;
// Expired lobbies a publish deletes, so a project does not fill with dead ones.
constexpr int kSweptPerPublish = 8;
// An ID token is refreshed this long before Firebase says it lapses.
constexpr std::int64_t kTokenMarginSeconds = 60;
// Hex digits of the lobby id's hash in its key: Firebase keys refuse . $ # [ ] /.
constexpr std::size_t kKeyHashDigits = 32;

net::Url url_or_throw(const std::string &text) {
  const std::optional<net::Url> url = net::parse_url(text);
  if (!url || (url->scheme != "http" && url->scheme != "https") ||
      url->target.find('?') != std::string::npos) {
    throw std::invalid_argument{"not a Firebase URL: " + text};
  }
  net::Url out = *url;
  while (!out.target.empty() && out.target.back() == '/') {
    out.target.pop_back();
  }
  return out;
}

const std::string *text_of(const json::Value &value, std::string_view key) {
  const json::Value *member = value.find(key);
  return member != nullptr ? member->string() : nullptr;
}

std::optional<std::int64_t> seconds_of(const json::Value &value, std::string_view key) {
  const json::Value *member = value.find(key);
  if (member == nullptr) {
    return std::nullopt;
  }
  // The token service sends its lifetime as a decimal string.
  if (const std::string *digits = member->string()) {
    std::string ignored;
    const std::optional<json::Value> number = json::parse(*digits, ignored);
    return number ? number->integer() : std::nullopt;
  }
  return member->integer();
}

std::string why(const std::string &body) {
  std::string ignored;
  const std::optional<json::Value> reply = json::parse(body, ignored);
  if (!reply) {
    return {};
  }
  const json::Value *error = reply->find("error");
  if (error == nullptr) {
    return {};
  }
  if (const std::string *text = error->string()) {
    return ": " + *text;
  }
  const std::string *message = text_of(*error, "message");
  return message != nullptr ? ": " + *message : std::string{};
}

} // namespace

FirebaseDirectory::FirebaseDirectory(const std::string &game, FirebaseProject project,
                                     TlsTrust trust)
    : game_{game}, project_{std::move(project)}, trust_{std::move(trust)},
      database_{url_or_throw(project_.database)} {
  if (!valid_game_name(game) || project_.api_key.empty()) {
    throw std::invalid_argument{"not a Firebase lobby game and key: " + game};
  }
  if (project_.auth_emulator) {
    const net::Url emulator = url_or_throw(*project_.auth_emulator);
    const std::string base = emulator.scheme + "://" + emulator.authority() + emulator.target;
    identity_base_ = base + "/identitytoolkit.googleapis.com";
    token_base_ = base + "/securetoken.googleapis.com";
  } else {
    identity_base_ = "https://identitytoolkit.googleapis.com";
    token_base_ = "https://securetoken.googleapis.com";
  }
}

std::optional<FirebaseDirectory::Reply>
FirebaseDirectory::send(const net::Url &url, std::string_view method,
                        const std::optional<std::string> &body, std::string_view content_type,
                        std::string &error) const {
  const std::unique_ptr<net::ByteStream> stream = connect_stream(url, trust_, error);
  if (!stream) {
    return std::nullopt;
  }
  const std::string headers =
      body ? "Content-Type: " + std::string{content_type} + "\r\n" : std::string{};
  http::FetchRequest request;
  request.method = method;
  request.target = url.target;
  request.headers = headers;
  if (body) {
    request.body = *body;
  }
  request.max_body = kMaxReplyBytes;
  http::FetchResult result;
  if (!http::fetch(*stream, url.authority(), request, result, error)) {
    return std::nullopt;
  }
  return Reply{result.status, std::move(result.body)};
}

bool FirebaseDirectory::signed_in(std::string &error) {
  const std::int64_t now = unix_seconds();
  if (!id_token_.empty() && now + kTokenMarginSeconds < token_expires_at_) {
    return true;
  }
  const bool refresh = !refresh_token_.empty();
  const std::string key = "?key=" + text::url_encode(project_.api_key);
  const std::optional<net::Url> url = net::parse_url(
      refresh ? token_base_ + "/v1/token" + key : identity_base_ + "/v1/accounts:signUp" + key);
  if (!url) {
    error = "no Firebase auth URL";
    return false;
  }
  const std::optional<Reply> reply =
      refresh ? send(*url, "POST",
                     "grant_type=refresh_token&refresh_token=" + text::url_encode(refresh_token_),
                     "application/x-www-form-urlencoded", error)
              : send(*url, "POST", std::string{"{\"returnSecureToken\":true}"}, "application/json",
                     error);
  if (!reply) {
    return false;
  }
  if (reply->status != 200) {
    error = "Firebase sign-in answered " + std::to_string(reply->status) + why(reply->body);
    // A refused refresh token is spent; the next attempt signs in afresh.
    refresh_token_.clear();
    id_token_.clear();
    return false;
  }
  const std::optional<json::Value> parsed = json::parse(reply->body, error);
  if (!parsed) {
    return false;
  }
  // signUp and the token service name the same fields differently.
  const std::string *token = text_of(*parsed, refresh ? "id_token" : "idToken");
  const std::string *renewal = text_of(*parsed, refresh ? "refresh_token" : "refreshToken");
  const std::string *user = text_of(*parsed, refresh ? "user_id" : "localId");
  const std::optional<std::int64_t> lifetime =
      seconds_of(*parsed, refresh ? "expires_in" : "expiresIn");
  if (token == nullptr || renewal == nullptr || user == nullptr || !lifetime || user->empty()) {
    error = "Firebase sign-in answered without a token";
    return false;
  }
  if (!user_.empty() && *user != user_) {
    log(Level::Info, "lobby", "Firebase gave this player a new user; earlier lobbies lapse");
  }
  id_token_ = *token;
  refresh_token_ = *renewal;
  user_ = *user;
  token_expires_at_ = now + *lifetime;
  return true;
}

std::optional<std::string> FirebaseDirectory::database(std::string_view method,
                                                       const Resource &resource,
                                                       const std::optional<std::string> &body,
                                                       std::string &error) {
  if (!signed_in(error)) {
    return std::nullopt;
  }
  net::Url url = database_;
  url.target += std::string{kRoot} + resource.path + ".json?auth=" + text::url_encode(id_token_);
  if (project_.emulator_namespace) {
    url.target += "&ns=" + text::url_encode(*project_.emulator_namespace);
  }
  url.target += resource.query;
  const std::optional<Reply> reply = send(url, method, body, "application/json", error);
  if (!reply) {
    return std::nullopt;
  }
  if (reply->status != 200) {
    error = "the Firebase database answered " + std::to_string(reply->status) + why(reply->body);
    return std::nullopt;
  }
  return reply->body;
}

std::string FirebaseDirectory::lobbies() const {
  return "lobbies/" + game_;
}

std::string FirebaseDirectory::mailbox_path(const std::string &mailbox) const {
  return "letters/" + game_ + "/" + mailbox;
}

std::string FirebaseDirectory::key_of(const std::string &id) const {
  const crypto::Bytes hash = crypto::sha256(crypto::bytes_of(id));
  return user_ + "-" + text::hex(hash).substr(0, kKeyHashDigits);
}

bool FirebaseDirectory::publish(const Lobby &lobby, std::string &error) {
  if (!publishable(lobby)) {
    error = "a lobby needs an id and a contact, fields within their limits and seats that add up";
    return false;
  }
  if (!signed_in(error)) {
    return false;
  }
  const std::int64_t now = unix_seconds();
  Lobby listed = lobby;
  listed.host = user_;
  listed.expires_at = now + kLobbySeconds;
  if (!database("PUT", {.path = lobbies() + "/" + key_of(lobby.id), .query = {}},
                lobby_json(listed, true), error)) {
    return false;
  }
  sweep(lobbies(), now);
  return true;
}

void FirebaseDirectory::sweep(const std::string &collection, std::int64_t now) {
  std::string error;
  const std::optional<std::string> body =
      database("GET",
               {.path = collection,
                .query = "&orderBy=" + text::url_encode("\"expires_at\"") +
                         "&endAt=" + std::to_string(now - 1) +
                         "&limitToFirst=" + std::to_string(kSweptPerPublish)},
               std::nullopt, error);
  const std::optional<json::Value> parsed =
      body ? json::parse(*body, error) : std::optional<json::Value>{};
  // Nothing expired reads back as null.
  if (parsed && parsed->is_null()) {
    return;
  }
  if (!parsed || parsed->object() == nullptr) {
    log(Level::Warn, "lobby",
        "no sweep of expired Firebase entries: " + (error.empty() ? "not a list" : error));
    return;
  }
  for (const auto &[key, entry] : *parsed->object()) {
    std::string path = collection;
    path += "/";
    path += key;
    if (!database("DELETE", {.path = std::move(path), .query = {}}, std::nullopt, error)) {
      log(Level::Warn, "lobby", "an expired Firebase entry stays: " + error);
    }
  }
}

bool FirebaseDirectory::withdraw(const std::string &id, std::string &error) {
  if (!signed_in(error)) {
    return false;
  }
  return database("DELETE", {.path = lobbies() + "/" + key_of(id), .query = {}}, std::nullopt,
                  error)
      .has_value();
}

std::optional<std::vector<Lobby>> FirebaseDirectory::list(std::string &error) {
  const std::int64_t now = unix_seconds();
  const std::optional<std::string> body =
      database("GET",
               {.path = lobbies(),
                .query = "&orderBy=" + text::url_encode("\"expires_at\"") +
                         "&startAt=" + std::to_string(now + 1) +
                         "&limitToLast=" + std::to_string(kMaxListedLobbies)},
               std::nullopt, error);
  if (!body) {
    return std::nullopt;
  }
  const std::optional<json::Value> parsed = json::parse(*body, error);
  if (!parsed) {
    return std::nullopt;
  }
  std::vector<Lobby> found;
  // An empty game reads back as null.
  if (parsed->is_null()) {
    return found;
  }
  if (parsed->object() == nullptr) {
    error = "the Firebase lobby list is not one";
    return std::nullopt;
  }
  for (const auto &[key, entry] : *parsed->object()) {
    std::optional<Lobby> lobby = lobby_of_json(entry);
    // The key is the rules' proof of who wrote it.
    if (lobby && !lobby->host.empty() && key.starts_with(lobby->host + "-") &&
        lobby->expires_at > now) {
      found.push_back(std::move(*lobby));
    }
  }
  std::sort(found.begin(), found.end(), [](const Lobby &left, const Lobby &right) {
    return left.expires_at > right.expires_at;
  });
  return found;
}

bool FirebaseDirectory::post(const std::string &mailbox, const std::string &letter,
                             std::string &error) {
  if (!valid_mailbox(mailbox) || letter.empty() || letter.size() > kMaxLetterTextBytes) {
    error = "not a mailbox and a letter";
    return false;
  }
  if (!signed_in(error)) {
    return false;
  }
  const std::int64_t now = unix_seconds();
  const std::string key = user_ + "-" + text::hex(crypto::random_bytes(kKeyHashDigits / 2));
  if (!database("PUT", {.path = mailbox_path(mailbox) + "/" + key, .query = {}},
                "{\"letter\":" + text::json_quote(letter) +
                    ",\"expires_at\":" + std::to_string(now + kLetterSeconds) + "}",
                error)) {
    return false;
  }
  sweep(mailbox_path(mailbox), now);
  return true;
}

std::optional<std::vector<std::string>> FirebaseDirectory::letters(const std::string &mailbox,
                                                                   std::string &error) {
  if (!valid_mailbox(mailbox)) {
    error = "not a mailbox";
    return std::nullopt;
  }
  const std::int64_t now = unix_seconds();
  const std::optional<std::string> body =
      database("GET",
               {.path = mailbox_path(mailbox),
                .query = "&orderBy=" + text::url_encode("\"expires_at\"") +
                         "&startAt=" + std::to_string(now + 1) +
                         "&limitToLast=" + std::to_string(kMaxLettersListed)},
               std::nullopt, error);
  if (!body) {
    return std::nullopt;
  }
  const std::optional<json::Value> parsed = json::parse(*body, error);
  if (!parsed) {
    return std::nullopt;
  }
  std::vector<std::pair<std::int64_t, std::string>> open;
  if (parsed->object() != nullptr) {
    for (const auto &[key, entry] : *parsed->object()) {
      const std::string *letter = text_of(entry, "letter");
      const std::optional<std::int64_t> expires = seconds_of(entry, "expires_at");
      if (letter != nullptr && !letter->empty() && letter->size() <= kMaxLetterTextBytes &&
          expires && *expires > now) {
        open.emplace_back(*expires, *letter);
      }
    }
  } else if (!parsed->is_null()) {
    error = "the Firebase letters are not a list";
    return std::nullopt;
  }
  std::sort(open.begin(), open.end(), [](const auto &left, const auto &right) {
    return left.first > right.first;
  });
  std::vector<std::string> out;
  out.reserve(open.size());
  for (auto &[expires, letter] : open) {
    out.push_back(std::move(letter));
  }
  return out;
}

std::string firebase_rules_json() {
  const auto text_field = [](std::size_t limit) {
    return "{\".validate\":\"newData.isString() && newData.val().length <= " +
           std::to_string(limit) + "\"}";
  };
  const auto expiry = [](std::int64_t lifetime) {
    return "{\".validate\":\"newData.isNumber() && newData.val() <= now / 1000 + " +
           std::to_string(2 * lifetime) + "\"}";
  };
  const std::string seats = "{\".validate\":\"newData.isNumber() && newData.val() >= 0 && "
                            "newData.val() <= " +
                            std::to_string(kMaxLobbyCapacity) + "\"}";
  // Removing an entry that expired is anyone's; writing one is only its user's.
  const std::string swept = "(!newData.exists() && data.child('expires_at').val() < now / 1000)";
  return std::string{"{\"rules\":{\"lucent\":{\"lobbies\":{\"$game\":{"} +
         "\".read\":\"auth != null\",\".indexOn\":[\"expires_at\"],\"$key\":{"
         "\".write\":\"auth != null && ($key.beginsWith(auth.uid + '-') || " +
         swept +
         ")\","
         "\".validate\":\"newData.hasChildren(['id','contact','host','expires_at']) && "
         "$game.length <= 32\","
         "\"id\":" +
         text_field(kMaxLobbyIdBytes) + ",\"title\":" + text_field(kMaxLobbyTitleBytes) +
         ",\"contact\":{\".validate\":\"newData.isString() && newData.val().length == 64\"}" +
         ",\"version\":" + text_field(kMaxLobbyVersionBytes) + ",\"players\":" + seats +
         ",\"capacity\":" + seats +
         ",\"host\":{\".validate\":\"newData.val() === auth.uid\"},"
         "\"expires_at\":" +
         expiry(kLobbySeconds) +
         ","
         "\"$other\":{\".validate\":false}}}},"
         "\"letters\":{\"$game\":{\"$mailbox\":{"
         "\".read\":\"auth != null\",\".indexOn\":[\"expires_at\"],\"$key\":{"
         "\".write\":\"auth != null && ((!data.exists() && $key.beginsWith(auth.uid + '-')) || " +
         swept +
         ")\","
         "\".validate\":\"newData.hasChildren(['letter','expires_at']) && $game.length <= 32 && "
         "$mailbox.length == 64\","
         "\"letter\":" +
         text_field(kMaxLetterTextBytes) + ",\"expires_at\":" + expiry(kLetterSeconds) +
         ",\"$other\":{\".validate\":false}}}}}}}}";
}

} // namespace lucent::p2p

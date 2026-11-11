#include "lucent/p2p/http_directory.h"

#include "lucent/http_client.h"
#include "lucent/json.h"
#include "lucent/text.h"

#include <stdexcept>

namespace lucent::p2p {
namespace {

// A board's list is at most this long.
constexpr std::size_t kMaxListBytes = std::size_t{2} * 1024 * 1024;

} // namespace

HttpDirectory::HttpDirectory(const std::string &game, const std::string &base, TlsTrust trust)
    : game_{game}, trust_{std::move(trust)}, token_{crypto::random_bytes(crypto::kKeyBytes)} {
  const std::optional<net::Url> url = net::parse_url(base);
  if (!valid_game_name(game) || !url || (url->scheme != "http" && url->scheme != "https") ||
      url->target.find('?') != std::string::npos) {
    throw std::invalid_argument{"not a lobby board game and base: " + game + " " + base};
  }
  base_ = *url;
  while (!base_.target.empty() && base_.target.back() == '/') {
    base_.target.pop_back();
  }
}

HttpDirectory::~HttpDirectory() {
  crypto::wipe(token_);
}

std::optional<std::string> HttpDirectory::request(std::string_view method, const std::string &path,
                                                  const std::optional<std::string> &body,
                                                  std::string &error) {
  const std::unique_ptr<net::ByteStream> stream = connect_stream(base_, trust_, error);
  if (!stream) {
    return std::nullopt;
  }
  const std::string target = base_.target + path;
  http::FetchRequest sent;
  sent.method = method;
  sent.target = target;
  sent.headers = body ? "Content-Type: application/json\r\n" : "";
  if (body) {
    sent.body = *body;
  }
  sent.max_body = kMaxListBytes;
  http::FetchResult result;
  if (!http::fetch(*stream, base_.authority(), sent, result, error)) {
    return std::nullopt;
  }
  if (result.status != 200) {
    std::string ignored;
    const std::optional<json::Value> reply = json::parse(result.body, ignored);
    const json::Value *why = reply ? reply->find("error") : nullptr;
    error = "the lobby board answered " + std::to_string(result.status) +
            (why != nullptr && why->string() != nullptr ? ": " + *why->string() : "");
    return std::nullopt;
  }
  return std::move(result.body);
}

bool HttpDirectory::publish(const Lobby &lobby, std::string &error) {
  if (!publishable(lobby)) {
    error = "a lobby needs an id and an invite, fields within their limits and seats that add up";
    return false;
  }
  return request("PUT", "/v1/lobbies",
                 "{\"game\":" + text::json_quote(game_) +
                     ",\"token\":" + text::json_quote(text::hex(token_)) +
                     ",\"lobby\":" + lobby_json(lobby, false) + "}",
                 error)
      .has_value();
}

bool HttpDirectory::withdraw(const std::string &id, std::string &error) {
  return request("POST", "/v1/lobbies/withdraw",
                 "{\"game\":" + text::json_quote(game_) + ",\"token\":" +
                     text::json_quote(text::hex(token_)) + ",\"id\":" + text::json_quote(id) + "}",
                 error)
      .has_value();
}

std::optional<std::vector<Lobby>> HttpDirectory::list(std::string &error) {
  const std::optional<std::string> body =
      request("GET", "/v1/lobbies?game=" + game_, std::nullopt, error);
  if (!body) {
    return std::nullopt;
  }
  const std::optional<json::Value> reply = json::parse(*body, error);
  const json::Value *lobbies = reply ? reply->find("lobbies") : nullptr;
  if (lobbies == nullptr || lobbies->array() == nullptr) {
    error = "the lobby board's list is not one";
    return std::nullopt;
  }
  std::vector<Lobby> found;
  for (const json::Value &entry : *lobbies->array()) {
    if (std::optional<Lobby> lobby = lobby_of_json(entry)) {
      found.push_back(std::move(*lobby));
    }
  }
  return found;
}

} // namespace lucent::p2p

// A lobby board on a loopback server with a stepped clock, reached by two HTTP directories: a
// published lobby is listed with its fields, only its own host withdraws it, it lapses unless
// published again, and malformed requests are refused.
#include "lucent/p2p/lobby_board.h"

#include "lucent/http_client.h"
#include "lucent/p2p/http_directory.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {

// How many lobbies `directory` lists, or -1 when the list fails.
int listed_count(lucent::p2p::Directory &directory) {
  std::string error;
  const auto lobbies = directory.list(error);
  return lobbies ? static_cast<int>(lobbies->size()) : -1;
}

} // namespace

int main() {
  std::atomic<std::int64_t> clock{1'800'000'000};
  lucent::p2p::LobbyBoard board{[&clock] {
    return clock.load();
  }};
  lucent::http::Server server{lucent::http::ServerOptions{},
                              [&board](const lucent::http::Request &request) {
                                return board.handle(request);
                              }};
  const bool started = server.start();
  assert(started);
  const std::string base = "http://127.0.0.1:" + std::to_string(server.port()) + "/";
  lucent::p2p::HttpDirectory host{"test", base, {}};
  lucent::p2p::HttpDirectory friend_{"test", base, {}};
  lucent::p2p::HttpDirectory other_game{"other", base, {}};

  lucent::p2p::Lobby lobby{"room", "Bar\xC3\xA9's room", "INVITE", 1, 4, "2.1", {}, 0};
  std::string error;
  const bool published = host.publish(lobby, error);
  assert(published);
  const auto listed = friend_.list(error);
  assert(listed && listed->size() == 1);
  const lucent::p2p::Lobby found = listed.value_or(std::vector<lucent::p2p::Lobby>{{}}).front();
  assert(found.id == "room" && found.title == lobby.title && found.invite == "INVITE" &&
         found.players == 1 && found.capacity == 4 && found.version == "2.1" &&
         found.host.size() == 64 && found.expires_at == clock + lucent::p2p::kBoardLobbySeconds);
  assert(listed_count(other_game) == 0);

  const bool not_theirs = friend_.withdraw("room", error);
  assert(not_theirs && listed_count(friend_) == 1);
  const bool withdrawn = host.withdraw("room", error);
  assert(withdrawn && listed_count(friend_) == 0);

  host.publish(lobby, error);
  clock += lucent::p2p::kBoardLobbySeconds - 1;
  assert(listed_count(friend_) == 1);
  clock += 1;
  assert(listed_count(friend_) == 0);

  lucent::http::FetchResult result;
  const auto ask = [&](std::string_view method, std::string_view target,
                       std::optional<std::string_view> body) {
    lucent::http::fetch(lucent::net::Endpoint{0x7F000001u, server.port()},
                        lucent::http::FetchRequest{method, target, {}, body}, result, error);
    return result.status;
  };
  assert(ask("PUT", "/v1/lobbies",
             R"({"game":"test","token":"zz","lobby":{"id":"a","title":"","invite":"x",)"
             R"("players":0,"capacity":1,"version":""}})") == 400);
  assert(ask("GET", "/v1/lobbies", std::nullopt) == 400);
  assert(ask("GET", "/v1/lobbies?game=Bad%20Name", std::nullopt) == 400);
  assert(ask("DELETE", "/v1/lobbies", std::nullopt) == 405);
  assert(ask("GET", "/elsewhere", std::nullopt) == 404);
  lobby.capacity = 0;
  assert(!host.publish(lobby, error));
  server.stop();
  assert(listed_count(friend_) == -1);

  bool thrown = false;
  try {
    lucent::p2p::HttpDirectory bad{"Bad Name", base, {}};
  } catch (const std::invalid_argument &) {
    thrown = true;
  }
  assert(thrown);
  std::puts("a lobby board lists, withdraws and lapses lobbies for their hosts only, and refuses "
            "malformed requests");
  return 0;
}

// A host lists a lobby on a loopback lobby board and players ask it for a seat: the listing holds
// the contact key and no invite, an asker gets the invite sealed to it alone, each key is heard
// once, a host takes kMaxAsksPerMinute asks a minute, "full" reaches the asker, an answer from
// another key is not taken, and a new listing has a new contact.
#include "lucent/p2p/lobby_seats.h"

#include "lucent/http.h"
#include "lucent/p2p/http_directory.h"
#include "lucent/p2p/letter.h"
#include "lucent/p2p/lobby_board.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace lucent::p2p;

std::vector<LobbyHost::Asker> asks_of(LobbyHost &host) {
  std::string error;
  std::optional<std::vector<LobbyHost::Asker>> asks = host.asks(error);
  assert(asks);
  return asks.value_or(std::vector<LobbyHost::Asker>{});
}

} // namespace

int main() {
  std::atomic<std::int64_t> clock{1'800'000'000};
  const UnixClock now = [&clock] {
    return clock.load();
  };
  LobbyBoard board{now};
  lucent::http::Server server{lucent::http::ServerOptions{},
                              [&board](const lucent::http::Request &request) {
                                return board.handle(request);
                              }};
  const bool started = server.start();
  assert(started);
  const std::string base = "http://127.0.0.1:" + std::to_string(server.port()) + "/";
  HttpDirectory host_directory{"test", base, {}};
  HttpDirectory guest_directory{"test", base, {}};
  std::string error;

  LobbyHost host{host_directory, Lobby{"room", "Room", {}, 1, 2, "1", {}, 0}, now};
  assert(valid_mailbox(host.lobby().contact));
  const bool listed = host.keep_listed(error);
  assert(listed);
  const auto lobbies = guest_directory.list(error);
  assert(lobbies && lobbies->size() == 1);
  const Lobby found = lobbies.value_or(std::vector<Lobby>{{}}).front();
  assert(found.contact == host.lobby().contact);
  assert(lobby_json(found, true).find("OSU") == std::string::npos);

  // One asker: the invite reaches it, sealed so that only it opens it.
  LobbyGuest guest{guest_directory, found};
  assert(guest.ask(error));
  assert(!guest.answer(error) && error.empty());
  std::vector<LobbyHost::Asker> asks = asks_of(host);
  assert(asks.size() == 1);
  assert(asks_of(host).empty());
  assert(host.answer(asks.front(), "OSU-INVITE", error));
  const auto answer = guest.answer(error);
  assert(answer && answer->invite == "OSU-INVITE");
  const auto in_box = guest_directory.letters(mailbox_of(asks.front().key), error);
  assert(in_box && in_box->size() == 1 && in_box->front().find("OSU") == std::string::npos);
  const crypto::KeyPair stranger;
  assert(!read_letter(stranger, in_box->front(), LetterPurpose::Answer));

  // The same key asking again is not heard again.
  assert(guest.ask(error));
  assert(asks_of(host).empty());

  // An answer from a key that is not the host's is not taken.
  LobbyGuest second{guest_directory, found};
  assert(second.ask(error));
  const std::vector<LobbyHost::Asker> second_asks = asks_of(host);
  assert(second_asks.size() == 1);
  const crypto::KeyPair impostor;
  assert(guest_directory.post(
      mailbox_of(second_asks.front().key),
      write_letter(impostor, second_asks.front().key, LetterPurpose::Answer, "invite\nFAKE"),
      error));
  assert(!second.answer(error));
  assert(host.answer(second_asks.front(), std::nullopt, error));
  const auto full = second.answer(error);
  assert(full && !full->invite);

  // Past the minute's asks, the rest wait for the next minute.
  clock += 61;
  std::vector<std::unique_ptr<LobbyGuest>> crowd;
  for (std::size_t index = 0; index < kMaxAsksPerMinute + 2; ++index) {
    crowd.push_back(std::make_unique<LobbyGuest>(guest_directory, found));
    assert(crowd.back()->ask(error));
  }
  assert(asks_of(host).size() == kMaxAsksPerMinute);
  assert(asks_of(host).empty());
  clock += 61;
  assert(asks_of(host).size() == 2);

  // Seats change the listing; withdrawing takes it off; a new listing has a new contact.
  host.seat(2);
  assert(host.keep_listed(error));
  const auto seated = guest_directory.list(error);
  assert(seated && seated->size() == 1 && seated->front().players == 2);
  assert(host.withdraw(error));
  const auto gone = guest_directory.list(error);
  assert(gone && gone->empty());
  LobbyHost again{host_directory, Lobby{"room", "Room", {}, 1, 2, "1", {}, 0}, now};
  assert(again.lobby().contact != host.lobby().contact);

  server.stop();
  std::puts("lobby seats: asks sealed to the contact, invites sealed to the asker, each key "
            "heard once, a minute's asks capped, full and impostor answers handled");
  return 0;
}

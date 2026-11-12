// The lab relay on a loopback server: it takes an event signed elsewhere and refuses forged,
// re-signed, expired and oversized ones; an addressable event replaces the last; a Nostr
// directory lists, withdraws and carries letters through it; and a player is seated through it.
#include "lucent/p2p/nostr_relay.h"

#include "lucent/http.h"
#include "lucent/p2p/crypto.h"
#include "lucent/p2p/lobby_seats.h"
#include "lucent/p2p/nostr_directory.h"
#include "lucent/websocket.h"

#include <cassert>
#include <cstdio>
#include <memory>
#include <string>

namespace {

using namespace lucent::p2p;

// From rust-nostr's tests (MIT, crates/nostr/src/event/mod.rs).
constexpr std::string_view kForeignEvent =
    R"({"id":"2be17aa3031bdcb006f0fce80c146dea9c1c0268b0af2398bb673365c6444d45",)"
    R"("pubkey":"f86c44a2de95d9149b51c6a29afeabba264c18e2fa7c49de93424a0c56947785",)"
    R"("created_at":1640839235,"kind":4,)"
    R"("tags":[["p","13adc511de7e1cfcf1c6b7f6365fb5a03442d7bcacf565ea57fa7770912c023d"]],)"
    R"("content":"uRuvYr585B80L6rSJiHocw==?iv=oh6LVqdsYYol3JfFnXTbPA==",)"
    R"("sig":"a5d9290ef9659083c490b303eb7ee41356d8778ff19f2f91776c8dc4443388a64ffcf336e61af4c25c05ac3ae952d1ced889ed655b67790891222aaa15b99fdd"})";

nostr::Event note(int kind, std::string d, std::int64_t created_at, std::string content) {
  nostr::Event event;
  event.created_at = created_at;
  event.kind = kind;
  event.tags = {{"d", std::move(d)}, {"t", "lucent-test"}};
  event.content = std::move(content);
  return event;
}

} // namespace

int main() {
  nostr::RelayStore store;
  lucent::http::Server server{lucent::http::ServerOptions{.max_connections = 32},
                              [&store](const lucent::http::Request &request) {
                                return lucent::http::websocket_upgrade(
                                    request, [&store](lucent::http::WebSocket &socket) {
                                      store.serve(socket);
                                    });
                              }};
  const bool started = server.start();
  assert(started);
  const std::string url = "ws://127.0.0.1:" + std::to_string(server.port());
  std::string error;
  std::unique_ptr<nostr::Relay> relay = nostr::Relay::connect(url, {}, error);
  assert(relay);

  const std::optional<lucent::json::Value> foreign = lucent::json::parse(kForeignEvent, error);
  const std::optional<nostr::Event> signed_elsewhere =
      foreign ? nostr::event_of(*foreign, error) : std::nullopt;
  assert(signed_elsewhere && relay->publish(*signed_elsewhere, error));

  const crypto::Bytes secret = crypto::random_bytes(32);
  const std::int64_t now = unix_seconds();
  nostr::Event forged = note(30078, "a", now, "one");
  nostr::sign(forged, secret);
  forged.content = "changed";
  assert(!relay->publish(forged, error) && error.find("invalid") != std::string::npos);
  nostr::Event resigned = note(30078, "a", now, "one");
  nostr::sign(resigned, secret);
  resigned.sig[5] = resigned.sig[5] == '0' ? '1' : '0';
  assert(!relay->publish(resigned, error));
  nostr::Event expired = note(30078, "b", now, "gone");
  expired.tags.push_back({"expiration", std::to_string(now - 1)});
  nostr::sign(expired, secret);
  assert(!relay->publish(expired, error));
  nostr::Event oversized = note(1, "c", now, std::string(nostr::kMaxRelayEventBytes + 1, 'x'));
  nostr::sign(oversized, secret);
  assert(!relay->publish(oversized, error));

  nostr::Event first = note(30078, "a", now - 5, "first");
  nostr::sign(first, secret);
  nostr::Event second = note(30078, "a", now, "second");
  nostr::sign(second, secret);
  assert(relay->publish(first, error) && relay->publish(second, error));
  assert(relay->publish(first, error));
  const auto kept = relay->query(R"({"kinds":[30078],"#t":["lucent-test"]})", 1024, error);
  assert(kept && kept->size() == 1 && kept->front().content == "second");
  const auto by_kind = relay->query(R"({"kinds":[4],"limit":5})", 1024, error);
  assert(by_kind && by_kind->size() == 1 && by_kind->front().id == signed_elsewhere->id);
  const auto none =
      relay->query("{\"kinds\":[30078],\"since\":" + std::to_string(now + 10) + "}", 1024, error);
  assert(none && none->empty());
  relay.reset();

  NostrDirectory host_directory{"test", {url}, {}};
  NostrDirectory guest_directory{"test", {url}, {}};
  LobbyHost host{host_directory, Lobby{"room", "Room", {}, 1, 2, "1", {}, 0}};
  assert(host.keep_listed(error));
  const auto lobbies = guest_directory.list(error);
  assert(lobbies && lobbies->size() == 1 && lobbies->front().contact == host.lobby().contact);
  LobbyGuest guest{guest_directory, lobbies->front()};
  assert(guest.ask(error));
  const auto asks = host.asks(error);
  assert(asks && asks->size() == 1);
  assert(host.answer(asks->front(), "INVITE", error));
  const auto answer = guest.answer(error);
  assert(answer && answer->invite == "INVITE");
  assert(host.withdraw(error));
  const auto gone = guest_directory.list(error);
  assert(gone && gone->empty());

  server.stop();
  std::puts("the lab relay checks every event as public relays do, replaces addressable ones, and "
            "seats a player through a Nostr directory");
  return 0;
}

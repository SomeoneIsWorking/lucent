// A Nostr directory over two loopback relay stand-ins and one dead relay: a published lobby is
// listed once with its fields, a withdrawn one is not, and forged, lapsed or other games' events
// never show.
#include "lucent/p2p/nostr_directory.h"

#include "lucent/p2p/nostr.h"
#include "lucent/text.h"
#include "websocket_peer.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace nostr = lucent::p2p::nostr;
namespace json = lucent::json;

// A relay that keeps the newest event per author and `d`, and answers every REQ with all of them
// and any extra events it was given, for a fixed number of connections.
class StandInRelay {
public:
  StandInRelay(std::vector<nostr::Event> extra, int connections)
      : extra_{std::move(extra)}, thread_{[this, connections] {
          serve(connections);
        }} {
  }
  ~StandInRelay() {
    thread_.join();
  }
  StandInRelay(const StandInRelay &) = delete;
  StandInRelay &operator=(const StandInRelay &) = delete;
  StandInRelay(StandInRelay &&) = delete;
  StandInRelay &operator=(StandInRelay &&) = delete;

  std::string url() const {
    return "ws://127.0.0.1:" + std::to_string(peer_.port());
  }

private:
  void serve(int connections) {
    for (int connection = 0; connection < connections; ++connection) {
      peer_.handshake(true);
      for (auto [opcode, text] = peer_.read_frame(); opcode == 0x1;
           std::tie(opcode, text) = peer_.read_frame()) {
        std::string error;
        const std::optional<json::Value> value = json::parse(text, error);
        assert(value && value->array() != nullptr);
        const json::Array &message = *value->array();
        if (*message[0].string() == "EVENT") {
          std::optional<nostr::Event> event = nostr::event_of(message[1], error);
          assert(event && nostr::verify(*event));
          std::erase_if(stored_, [&](const nostr::Event &kept) {
            return kept.pubkey == event->pubkey && kept.tag("d") == event->tag("d");
          });
          peer_.frame(0x81, "[\"OK\"," + lucent::text::json_quote(event->id) + ",true,\"\"]");
          stored_.push_back(std::move(*event));
        } else if (*message[0].string() == "REQ") {
          const std::string sub = lucent::text::json_quote(*message[1].string());
          for (const std::vector<nostr::Event> *events : {&stored_, &extra_}) {
            for (const nostr::Event &event : *events) {
              peer_.frame(0x81, "[\"EVENT\"," + sub + "," + nostr::to_json(event) + "]");
            }
          }
          peer_.frame(0x81, "[\"EOSE\"," + sub + "]");
        }
      }
    }
  }

  lucent::test::WebSocketPeer peer_;
  std::vector<nostr::Event> stored_;
  std::vector<nostr::Event> extra_;
  std::thread thread_;
};

std::int64_t now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

nostr::Event signed_lobby(std::string topic, std::int64_t expires, std::string invite) {
  nostr::Event event;
  event.created_at = now();
  event.kind = lucent::p2p::kNostrLobbyKind;
  event.tags = {{"d", "other"}, {"t", std::move(topic)}, {"expiration", std::to_string(expires)}};
  event.content = "{\"title\":\"x\",\"invite\":\"" + std::move(invite) +
                  "\",\"players\":1,\"capacity\":2,\"version\":\"1\"}";
  nostr::sign(event, lucent::p2p::crypto::random_bytes(32));
  return event;
}

} // namespace

int main() {
  nostr::Event forged = signed_lobby("lucent-test", now() + 100, "FORGED");
  forged.content = "{\"title\":\"x\",\"invite\":\"CHANGED\",\"players\":1,\"capacity\":2,"
                   "\"version\":\"1\"}";
  const std::vector<nostr::Event> extra{signed_lobby("lucent-other", now() + 100, "OTHER"),
                                        signed_lobby("lucent-test", now() - 1, "LAPSED"), forged};
  // publish, list, withdraw, list.
  StandInRelay first{{}, 4};
  StandInRelay second{extra, 4};
  lucent::p2p::NostrDirectory directory{
      "test", {first.url(), second.url(), "ws://127.0.0.1:1"}, lucent::p2p::TlsTrust{}};

  lucent::p2p::Lobby lobby;
  lobby.id = "room";
  lobby.title = "Bar\xC3\xA9's room";
  lobby.invite = "INVITE-CODE";
  lobby.players = 1;
  lobby.capacity = 4;
  lobby.version = "2.1";
  std::string error;
  const bool published = directory.publish(lobby, error);
  assert(published);

  const auto listed = directory.list(error);
  assert(listed && listed->size() == 1);
  const lucent::p2p::Lobby &found = listed->front();
  assert(found.id == "room" && found.title == lobby.title && found.invite == "INVITE-CODE" &&
         found.players == 1 && found.capacity == 4 && found.version == "2.1" &&
         found.host.size() == 64 && found.expires_at > now());

  const bool withdrawn = directory.withdraw("room", error);
  assert(withdrawn);
  const auto after = directory.list(error);
  assert(after && after->empty());

  lucent::p2p::NostrDirectory nowhere{"test", {"ws://127.0.0.1:1"}, lucent::p2p::TlsTrust{}};
  assert(!nowhere.publish(lobby, error) && error.find("no relay took") != std::string::npos);
  assert(!nowhere.list(error) && error.find("no relay answered") != std::string::npos);
  std::puts("a Nostr directory lists a published lobby once and drops withdrawn, lapsed, forged "
            "and other games' lobbies");
  return 0;
}

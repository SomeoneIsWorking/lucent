// Nostr events sign, verify and read back; a relay client against a loopback stand-in publishes,
// reads a refusal, and queries to EOSE while dropping a forged event.
#include "lucent/p2p/nostr.h"

#include "lucent/p2p/crypto.h"
#include "lucent/text.h"
#include "websocket_peer.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <thread>

namespace {

namespace nostr = lucent::p2p::nostr;
namespace json = lucent::json;

nostr::Event lobby_event() {
  nostr::Event event;
  event.created_at = 1731340800;
  event.kind = 30078;
  event.tags = {{"d", "lobby-1"}, {"t", "lucent-test"}, {"expiration", "1731341100"}};
  event.content = "{\"title\":\"Bar\\u00e9\"}\n\t\"q\"";
  return event;
}

json::Array message_of(const std::string &text) {
  std::string error;
  std::optional<json::Value> value = json::parse(text, error);
  assert(value && value->array() != nullptr);
  return *value->array();
}

void test_events() {
  nostr::Event event = lobby_event();
  event.pubkey = "ab";
  assert(
      nostr::canonical(event) ==
      "[0,\"ab\",1731340800,30078,[[\"d\",\"lobby-1\"],[\"t\",\"lucent-test\"],"
      "[\"expiration\",\"1731341100\"]],\"{\\\"title\\\":\\\"Bar\\\\u00e9\\\"}\\n\\t\\\"q\\\"\"]");

  const lucent::p2p::crypto::Bytes secret = lucent::p2p::crypto::random_bytes(32);
  assert(nostr::sign(event, secret) && nostr::verify(event));
  const auto public_key = lucent::p2p::crypto::schnorr_public_key(secret);
  assert(public_key && event.pubkey == lucent::text::hex(*public_key));
  assert(event.tag("expiration") == "1731341100" && !event.tag("missing"));

  std::string error;
  const std::optional<json::Value> value = json::parse(nostr::to_json(event), error);
  assert(value);
  const std::optional<nostr::Event> back = nostr::event_of(*value, error);
  assert(back && back->id == event.id && back->sig == event.sig && back->tags == event.tags &&
         back->content == event.content && nostr::verify(*back));

  nostr::Event changed = event;
  changed.content += " ";
  assert(!nostr::verify(changed));
  changed = event;
  changed.id = nostr::Event{}.id;
  assert(!nostr::verify(changed));
  const lucent::p2p::crypto::Bytes bad(32, 0);
  assert(!nostr::sign(changed, bad));
}

void test_relay() {
  const lucent::p2p::crypto::Bytes secret = lucent::p2p::crypto::random_bytes(32);
  nostr::Event published = lobby_event();
  nostr::sign(published, secret);
  nostr::Event refused = lobby_event();
  refused.content = "second";
  nostr::sign(refused, secret);
  nostr::Event forged = published;
  forged.content = "forged";

  lucent::test::WebSocketPeer peer;
  std::thread relay{[&] {
    peer.handshake(true);
    const json::Array first = message_of(peer.read_frame().second);
    assert(*first[0].string() == "EVENT");
    const std::string id = *first[1].find("id")->string();
    peer.frame(0x81, "[\"NOTICE\",\"hello\"]");
    peer.frame(0x81, "[\"OK\",\"" + id + "\",true,\"\"]");

    const json::Array second = message_of(peer.read_frame().second);
    peer.frame(0x81, "[\"OK\"," + lucent::text::json_quote(*second[1].find("id")->string()) +
                         ",false,\"blocked: a test\"]");

    const json::Array request = message_of(peer.read_frame().second);
    assert(*request[0].string() == "REQ" && request[2].find("kinds") != nullptr);
    const std::string sub = lucent::text::json_quote(*request[1].string());
    peer.frame(0x81, "[\"EVENT\",\"someone-else\"," + nostr::to_json(forged) + "]");
    peer.frame(0x81, "[\"EVENT\"," + sub + "," + nostr::to_json(forged) + "]");
    peer.frame(0x81, "[\"EVENT\"," + sub + "," + nostr::to_json(published) + "]");
    peer.frame(0x81, "[\"EOSE\"," + sub + "]");
    const json::Array close = message_of(peer.read_frame().second);
    assert(*close[0].string() == "CLOSE" && lucent::text::json_quote(*close[1].string()) == sub);
  }};

  std::string error;
  const auto client = nostr::Relay::connect("ws://127.0.0.1:" + std::to_string(peer.port()),
                                            lucent::p2p::TlsTrust{}, error);
  assert(client && error.empty());
  const bool accepted = client->publish(published, error);
  assert(accepted);
  assert(!client->publish(refused, error) && error.find("blocked: a test") != std::string::npos);
  error.clear();
  const auto found = client->query(R"({"kinds":[30078],"#t":["lucent-test"]})", error);
  assert(found && found->size() == 1 && found->front().id == published.id);
  relay.join();
  assert(peer.ok());

  assert(!nostr::Relay::connect("https://relay.example", lucent::p2p::TlsTrust{}, error));
}

} // namespace

int main() {
  test_events();
  test_relay();
  std::puts("Nostr events sign and verify, and a relay publishes, refuses and answers a query");
  return 0;
}

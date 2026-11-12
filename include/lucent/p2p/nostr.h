#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lucent/http_client.h"
#include "lucent/json.h"
#include "lucent/p2p/tls_stream.h"

// Nostr (NIP-01) events and one relay connection, for lobby discovery.
namespace lucent::p2p::nostr {

using Tag = std::vector<std::string>;

struct Event {
  // Lowercase hex: the id and key 32 bytes, the signature 64.
  std::string id;
  std::string pubkey;
  std::int64_t created_at = 0;
  int kind = 0;
  std::vector<Tag> tags;
  std::string content;
  std::string sig;

  // The value of the first tag named `name`, or nothing.
  std::optional<std::string_view> tag(std::string_view name) const;
};

// [0,pubkey,created_at,kind,tags,content], the text an event's id is the SHA-256 of.
std::string canonical(const Event &event);
std::string to_json(const Event &event);
// The event an object holds; nothing, with `error`, for a missing or mistyped field.
std::optional<Event> event_of(const json::Value &value, std::string &error);

// Sets the event's pubkey, id and sig from a 32-byte secret key; false for a bad key.
bool sign(Event &event, std::span<const std::uint8_t> secret);
// The id is the hash of the event and the signature is the pubkey's over it.
bool verify(const Event &event);

// One relay at a ws:// or wss:// URL. Each wait for the relay is the stream's 5 s.
class Relay {
public:
  static std::unique_ptr<Relay> connect(std::string_view url, const TlsTrust &trust,
                                        std::string &error);
  ~Relay() = default;
  Relay(const Relay &) = delete;
  Relay &operator=(const Relay &) = delete;
  Relay(Relay &&) = delete;
  Relay &operator=(Relay &&) = delete;

  // Sends the event and waits for the relay's OK; false with the relay's reason when refused.
  bool publish(const Event &event, std::string &error);
  // The stored events matching `filter` (a NIP-01 filter object as JSON), up to EOSE. Events
  // with content longer than `max_content`, or whose id or signature do not check out, are left
  // out; the length is checked first.
  std::optional<std::vector<Event>> query(std::string_view filter, std::size_t max_content,
                                          std::string &error);

private:
  explicit Relay(std::unique_ptr<http::WebSocketClient> socket) noexcept;
  // The next relay message, as a JSON array.
  std::optional<json::Array> receive(std::string &error);
  std::vector<Event> end_query(const std::string &subscription, std::vector<Event> events);

  std::unique_ptr<http::WebSocketClient> socket_;
  std::uint64_t subscriptions_ = 0;
};

} // namespace lucent::p2p::nostr

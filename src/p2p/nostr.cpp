#include "lucent/p2p/nostr.h"

#include "lucent/url.h"
#include "lucent/p2p/crypto.h"
#include "lucent/text.h"

namespace lucent::p2p::nostr {
namespace {

constexpr std::size_t kKeyHexDigits = 64;
constexpr std::size_t kSignatureHexDigits = 128;
// A relay that sends more than this before the answer asked for is dropped.
constexpr int kMaxMessagesBeforeAnswer = 256;
constexpr std::size_t kMaxEventsPerQuery = 1000;

std::string tags_json(const std::vector<Tag> &tags) {
  std::string out = "[";
  for (std::size_t index = 0; index < tags.size(); ++index) {
    out += index == 0 ? "[" : ",[";
    for (std::size_t part = 0; part < tags[index].size(); ++part) {
      if (part != 0) {
        out.push_back(',');
      }
      out += text::json_quote(tags[index][part]);
    }
    out.push_back(']');
  }
  out.push_back(']');
  return out;
}

std::string id_of(const Event &event) {
  return text::hex(crypto::sha256(crypto::bytes_of(canonical(event))));
}

const std::string *string_member(const json::Value &value, std::string_view key) {
  const json::Value *member = value.find(key);
  return member != nullptr ? member->string() : nullptr;
}

} // namespace

std::optional<std::string_view> Event::tag(std::string_view name) const {
  for (const Tag &entry : tags) {
    if (entry.size() >= 2 && entry[0] == name) {
      return entry[1];
    }
  }
  return std::nullopt;
}

std::string canonical(const Event &event) {
  return "[0," + text::json_quote(event.pubkey) + "," + std::to_string(event.created_at) + "," +
         std::to_string(event.kind) + "," + tags_json(event.tags) + "," +
         text::json_quote(event.content) + "]";
}

std::string to_json(const Event &event) {
  return "{\"id\":" + text::json_quote(event.id) + ",\"pubkey\":" + text::json_quote(event.pubkey) +
         ",\"created_at\":" + std::to_string(event.created_at) +
         ",\"kind\":" + std::to_string(event.kind) + ",\"tags\":" + tags_json(event.tags) +
         ",\"content\":" + text::json_quote(event.content) +
         ",\"sig\":" + text::json_quote(event.sig) + "}";
}

std::optional<Event> event_of(const json::Value &value, std::string &error) {
  Event event;
  const std::string *id = string_member(value, "id");
  const std::string *pubkey = string_member(value, "pubkey");
  const std::string *content = string_member(value, "content");
  const std::string *sig = string_member(value, "sig");
  const json::Value *created_at = value.find("created_at");
  const json::Value *kind = value.find("kind");
  const json::Value *tags = value.find("tags");
  const std::optional<std::int64_t> created =
      created_at != nullptr ? created_at->integer() : std::nullopt;
  const std::optional<std::int64_t> kind_number = kind != nullptr ? kind->integer() : std::nullopt;
  if (id == nullptr || pubkey == nullptr || content == nullptr || sig == nullptr || !created ||
      !kind_number || *kind_number < 0 || *kind_number > 65535 || tags == nullptr ||
      tags->array() == nullptr) {
    error = "an event lacks a field or has one of the wrong type";
    return std::nullopt;
  }
  for (const json::Value &entry : *tags->array()) {
    if (entry.array() == nullptr) {
      error = "an event tag is not an array";
      return std::nullopt;
    }
    Tag tag;
    for (const json::Value &part : *entry.array()) {
      if (part.string() == nullptr) {
        error = "an event tag holds a non-string";
        return std::nullopt;
      }
      tag.push_back(*part.string());
    }
    event.tags.push_back(std::move(tag));
  }
  event.id = *id;
  event.pubkey = *pubkey;
  event.created_at = *created;
  event.kind = static_cast<int>(*kind_number);
  event.content = *content;
  event.sig = *sig;
  return event;
}

bool sign(Event &event, std::span<const std::uint8_t> secret) {
  const std::optional<crypto::Bytes> public_key = crypto::schnorr_public_key(secret);
  if (!public_key) {
    return false;
  }
  event.pubkey = text::hex(*public_key);
  event.id = id_of(event);
  const std::optional<std::vector<std::uint8_t>> id = text::from_hex(event.id);
  const std::optional<crypto::Bytes> signature =
      id ? crypto::schnorr_sign(secret, *id, crypto::random_bytes(32)) : std::nullopt;
  if (!signature) {
    return false;
  }
  event.sig = text::hex(*signature);
  return true;
}

bool verify(const Event &event) {
  if (event.pubkey.size() != kKeyHexDigits || event.id.size() != kKeyHexDigits ||
      event.sig.size() != kSignatureHexDigits || id_of(event) != event.id) {
    return false;
  }
  const auto public_key = text::from_hex(event.pubkey);
  const auto id = text::from_hex(event.id);
  const auto signature = text::from_hex(event.sig);
  return public_key && id && signature && crypto::schnorr_verify(*public_key, *id, *signature);
}

Relay::Relay(std::unique_ptr<http::WebSocketClient> socket) noexcept : socket_{std::move(socket)} {
}

std::unique_ptr<Relay> Relay::connect(std::string_view url, const TlsTrust &trust,
                                      std::string &error) {
  const std::optional<net::Url> address = net::parse_url(url);
  if (!address || (address->scheme != "ws" && address->scheme != "wss")) {
    error = "not a ws:// or wss:// relay URL";
    return nullptr;
  }
  std::unique_ptr<net::ByteStream> stream = connect_stream(*address, trust, error);
  if (!stream) {
    return nullptr;
  }
  auto socket = std::make_unique<http::WebSocketClient>();
  if (!socket->connect(std::move(stream), address->authority(), address->target, error)) {
    return nullptr;
  }
  return std::unique_ptr<Relay>{new Relay{std::move(socket)}};
}

std::optional<json::Array> Relay::receive(std::string &error) {
  const std::optional<std::string> text = socket_->receive_text(error);
  if (!text) {
    return std::nullopt;
  }
  std::optional<json::Value> message = json::parse(*text, error);
  if (!message || message->array() == nullptr || message->array()->empty() ||
      (*message->array())[0].string() == nullptr) {
    if (error.empty()) {
      error = "the relay sent a message that is not a labelled array";
    }
    return std::nullopt;
  }
  return *message->array();
}

bool Relay::publish(const Event &event, std::string &error) {
  if (!socket_->send_text("[\"EVENT\"," + to_json(event) + "]", error)) {
    return false;
  }
  for (int count = 0; count < kMaxMessagesBeforeAnswer; ++count) {
    const std::optional<json::Array> message = receive(error);
    if (!message) {
      return false;
    }
    // ["OK", id, accepted, reason]
    if (*message->front().string() != "OK" || message->size() < 3 ||
        (*message)[1].string() == nullptr || *(*message)[1].string() != event.id) {
      continue;
    }
    const bool *accepted = (*message)[2].boolean();
    if (accepted != nullptr && *accepted) {
      return true;
    }
    const std::string *reason = message->size() > 3 ? (*message)[3].string() : nullptr;
    error = "the relay refused the event: " + (reason != nullptr ? *reason : std::string{"(none)"});
    return false;
  }
  error = "the relay never answered the event";
  return false;
}

std::vector<Event> Relay::end_query(const std::string &subscription, std::vector<Event> events) {
  // The events are in hand whether or not the relay takes the CLOSE.
  std::string ignored;
  socket_->send_text("[\"CLOSE\"," + text::json_quote(subscription) + "]", ignored);
  return events;
}

std::optional<std::vector<Event>> Relay::query(std::string_view filter, std::size_t max_content,
                                               std::string &error) {
  const std::string subscription = "lucent-" + std::to_string(++subscriptions_);
  if (!socket_->send_text(
          "[\"REQ\"," + text::json_quote(subscription) + "," + std::string{filter} + "]", error)) {
    return std::nullopt;
  }
  std::vector<Event> events;
  int unrelated = 0;
  while (unrelated < kMaxMessagesBeforeAnswer) {
    const std::optional<json::Array> message = receive(error);
    if (!message) {
      return std::nullopt;
    }
    const std::string &label = *message->front().string();
    const bool ours = message->size() >= 2 && (*message)[1].string() != nullptr &&
                      *(*message)[1].string() == subscription;
    if (!ours) {
      ++unrelated;
      continue;
    }
    if (label == "EOSE") {
      return end_query(subscription, std::move(events));
    }
    if (label == "CLOSED") {
      const std::string *reason = message->size() > 2 ? (*message)[2].string() : nullptr;
      error = "the relay closed the query: " + (reason != nullptr ? *reason : std::string{});
      return std::nullopt;
    }
    if (label != "EVENT" || message->size() < 3) {
      ++unrelated;
      continue;
    }
    std::string ignored;
    std::optional<Event> event = event_of((*message)[2], ignored);
    if (!event || event->content.size() > max_content || !verify(*event)) {
      ++unrelated;
      continue;
    }
    events.push_back(std::move(*event));
    if (events.size() == kMaxEventsPerQuery) {
      return end_query(subscription, std::move(events));
    }
  }
  error = "the relay never ended the query";
  return std::nullopt;
}

} // namespace lucent::p2p::nostr

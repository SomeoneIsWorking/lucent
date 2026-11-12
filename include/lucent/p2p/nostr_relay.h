#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "lucent/json.h"
#include "lucent/p2p/nostr.h"
#include "lucent/websocket.h"

// A Nostr relay for a lab with no internet (`lucent_nostr_relay`): the part of NIP-01 Lucent's
// directories use. Every EVENT's id and signature are checked as public relays check them, an
// addressable event (kinds 30000-39999) replaces the one with the same author, kind and `d`, and an
// event past its NIP-40 expiry is refused and never returned. REQ filters take `ids`, `authors`,
// `kinds`, `#<letter>` tags, `since`, `until` and `limit`, newest first.
namespace lucent::p2p::nostr {

inline constexpr std::size_t kMaxRelayEvents = 10000;
inline constexpr std::size_t kMaxRelayEventBytes = std::size_t{64} * 1024;

class RelayStore {
public:
  using Clock = std::function<std::int64_t()>;

  // `now` gives Unix seconds; the system clock unless a test steps one.
  explicit RelayStore(Clock now = {});

  // Serves one client until it closes or goes quiet.
  void serve(http::WebSocket &socket);

  // Takes `event`; false with the reason a relay's OK carries.
  bool accept(const Event &event, std::string &reason);
  // The stored events any of `filters` matches, newest first, each filter's `limit` applied.
  std::vector<Event> query(const json::Array &filters);

private:
  struct Stored {
    Event event;
    std::int64_t expires_at = 0;
  };

  void prune(std::int64_t now);

  Clock now_;
  std::mutex mutex_;
  std::vector<Stored> events_;
};

} // namespace lucent::p2p::nostr

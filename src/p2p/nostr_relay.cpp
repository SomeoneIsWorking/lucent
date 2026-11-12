#include "lucent/p2p/nostr_relay.h"

#include "lucent/p2p/directory.h"
#include "lucent/text.h"

#include <algorithm>
#include <charconv>
#include <optional>
#include <utility>

namespace lucent::p2p::nostr {
namespace {

constexpr std::size_t kMaxFilterLimit = 1000;

std::optional<std::int64_t> integer_of(std::string_view digits) {
  std::int64_t value = 0;
  const auto [end, problem] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
  if (problem != std::errc{} || end != digits.data() + digits.size()) {
    return std::nullopt;
  }
  return value;
}

bool addressable(int kind) noexcept {
  return kind >= 30000 && kind < 40000;
}

// Whether `event` is one of the strings in `list`, compared by `field`.
bool listed(const json::Value *list, const std::string &value) {
  if (list == nullptr) {
    return true;
  }
  if (list->array() == nullptr) {
    return false;
  }
  return std::any_of(list->array()->begin(), list->array()->end(), [&](const json::Value &item) {
    return item.string() != nullptr && *item.string() == value;
  });
}

bool matches(const json::Value &filter, const Event &event) {
  const json::Object *members = filter.object();
  if (members == nullptr) {
    return false;
  }
  for (const auto &[key, value] : *members) {
    if (key == "ids" && !listed(&value, event.id)) {
      return false;
    }
    if (key == "authors" && !listed(&value, event.pubkey)) {
      return false;
    }
    if (key == "kinds") {
      if (value.array() == nullptr ||
          std::none_of(value.array()->begin(), value.array()->end(), [&](const json::Value &kind) {
            return kind.integer() == event.kind;
          })) {
        return false;
      }
    }
    if (key == "since" && value.integer().value_or(0) > event.created_at) {
      return false;
    }
    if (key == "until" && value.integer().value_or(INT64_MAX) < event.created_at) {
      return false;
    }
    if (key.size() == 2 && key[0] == '#') {
      const std::string name(1, key[1]);
      const bool any = std::any_of(event.tags.begin(), event.tags.end(), [&](const Tag &tag) {
        return tag.size() >= 2 && tag[0] == name && listed(&value, tag[1]);
      });
      if (!any) {
        return false;
      }
    }
  }
  return true;
}

std::string notice(std::string_view text) {
  return "[\"NOTICE\"," + text::json_quote(text) + "]";
}

} // namespace

RelayStore::RelayStore(Clock now) : now_{now ? std::move(now) : Clock{unix_seconds}} {
}

void RelayStore::prune(std::int64_t now) {
  std::erase_if(events_, [now](const Stored &stored) {
    return stored.expires_at != 0 && stored.expires_at <= now;
  });
}

bool RelayStore::accept(const Event &event, std::string &reason) {
  if (event.content.size() > kMaxRelayEventBytes) {
    reason = "invalid: the event is too large";
    return false;
  }
  if (!verify(event)) {
    reason = "invalid: the id or signature does not check out";
    return false;
  }
  std::int64_t expires = 0;
  if (const std::optional<std::string_view> expiration = event.tag("expiration")) {
    const std::optional<std::int64_t> when = integer_of(*expiration);
    if (!when) {
      reason = "invalid: the expiration is not a number";
      return false;
    }
    expires = *when;
  }
  const std::int64_t now = now_();
  if (expires != 0 && expires <= now) {
    reason = "invalid: the event has expired";
    return false;
  }
  const std::lock_guard lock{mutex_};
  prune(now);
  if (addressable(event.kind)) {
    const std::string_view d = event.tag("d").value_or("");
    for (const Stored &stored : events_) {
      const Event &kept = stored.event;
      if (kept.kind == event.kind && kept.pubkey == event.pubkey &&
          kept.tag("d").value_or("") == d && kept.created_at > event.created_at) {
        reason = "duplicate: a newer event replaces it";
        return true;
      }
    }
    std::erase_if(events_, [&](const Stored &stored) {
      return stored.event.kind == event.kind && stored.event.pubkey == event.pubkey &&
             stored.event.tag("d").value_or("") == d;
    });
  }
  if (std::any_of(events_.begin(), events_.end(), [&](const Stored &stored) {
        return stored.event.id == event.id;
      })) {
    reason = "duplicate: already have it";
    return true;
  }
  if (events_.size() >= kMaxRelayEvents) {
    events_.erase(events_.begin());
  }
  events_.push_back(Stored{.event = event, .expires_at = expires});
  reason.clear();
  return true;
}

std::vector<Event> RelayStore::query(const json::Array &filters) {
  const std::lock_guard lock{mutex_};
  prune(now_());
  std::vector<const Stored *> newest;
  newest.reserve(events_.size());
  for (const Stored &stored : events_) {
    newest.push_back(&stored);
  }
  std::stable_sort(newest.begin(), newest.end(), [](const Stored *first, const Stored *second) {
    return first->event.created_at > second->event.created_at;
  });
  std::vector<Event> found;
  std::vector<char> taken(newest.size(), 0);
  for (const json::Value &filter : filters) {
    const json::Value *limit = filter.find("limit");
    const std::size_t most = limit != nullptr && limit->integer() && *limit->integer() >= 0
                                 ? std::min<std::size_t>(*limit->integer(), kMaxFilterLimit)
                                 : kMaxFilterLimit;
    std::size_t count = 0;
    for (std::size_t index = 0; index < newest.size() && count < most; ++index) {
      if (matches(filter, newest[index]->event)) {
        ++count;
        if (taken[index] == 0) {
          taken[index] = 1;
          found.push_back(newest[index]->event);
        }
      }
    }
  }
  std::stable_sort(found.begin(), found.end(), [](const Event &first, const Event &second) {
    return first.created_at > second.created_at;
  });
  return found;
}

void RelayStore::serve(http::WebSocket &socket) {
  std::string error;
  while (std::optional<std::string> text = socket.receive_text(error)) {
    std::string ignored;
    const std::optional<json::Value> message =
        text->size() <= 2 * kMaxRelayEventBytes ? json::parse(*text, ignored) : std::nullopt;
    const json::Array *parts = message ? message->array() : nullptr;
    const std::string *label =
        parts != nullptr && !parts->empty() ? parts->front().string() : nullptr;
    if (label == nullptr) {
      socket.send_text(notice("not a Nostr message"), error);
      continue;
    }
    if (*label == "EVENT" && parts->size() == 2) {
      std::optional<Event> event = event_of((*parts)[1], ignored);
      if (!event) {
        socket.send_text(notice("not an event"), error);
        continue;
      }
      std::string reason;
      const bool ok = accept(*event, reason);
      socket.send_text("[\"OK\"," + text::json_quote(event->id) + "," + (ok ? "true" : "false") +
                           "," + text::json_quote(reason) + "]",
                       error);
    } else if (*label == "REQ" && parts->size() >= 3 && (*parts)[1].string() != nullptr) {
      const std::string subscription = text::json_quote(*(*parts)[1].string());
      const json::Array filters(parts->begin() + 2, parts->end());
      for (const Event &event : query(filters)) {
        socket.send_text("[\"EVENT\"," + subscription + "," + to_json(event) + "]", error);
      }
      socket.send_text("[\"EOSE\"," + subscription + "]", error);
    } else if (*label == "CLOSE") {
      continue;
    } else {
      socket.send_text(notice("unsupported message"), error);
    }
  }
}

} // namespace lucent::p2p::nostr

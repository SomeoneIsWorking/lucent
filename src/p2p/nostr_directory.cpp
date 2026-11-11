#include "lucent/p2p/nostr_directory.h"

#include "lucent/json.h"
#include "lucent/log.h"
#include "lucent/p2p/nostr.h"
#include "lucent/text.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <functional>
#include <map>
#include <stdexcept>
#include <thread>
#include <utility>

namespace lucent::p2p {
namespace {

// A withdrawn lobby is replaced by an empty one that lapses after this.
constexpr std::int64_t kWithdrawnSeconds = 60;
constexpr int kMaxListedLobbies = 200;

std::int64_t now_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::optional<std::int64_t> integer_of(std::string_view digits) {
  std::int64_t value = 0;
  const auto [end, problem] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
  if (problem != std::errc{} || end != digits.data() + digits.size()) {
    return std::nullopt;
  }
  return value;
}

// The open lobby an event announces; nothing for a withdrawn, lapsed or malformed one.
std::optional<Lobby> lobby_of(const nostr::Event &event, std::int64_t now) {
  const std::optional<std::string_view> id = event.tag("d");
  const std::optional<std::string_view> expiration = event.tag("expiration");
  const std::optional<std::int64_t> expires = expiration ? integer_of(*expiration) : std::nullopt;
  if (!id || !expires || *expires <= now) {
    return std::nullopt;
  }
  std::string error;
  const std::optional<json::Value> content = json::parse(event.content, error);
  std::optional<Lobby> lobby = content ? lobby_of_json(*content) : std::nullopt;
  if (!lobby || lobby->id != *id) {
    return std::nullopt;
  }
  lobby->host = event.pubkey;
  lobby->expires_at = *expires;
  return lobby;
}

// Runs `work` once per relay index, all at once, and returns each relay's error ("" when it
// worked).
std::vector<std::string> each_relay(const std::vector<std::string> &relays,
                                    const std::function<bool(std::size_t, std::string &)> &work) {
  std::vector<std::string> errors(relays.size());
  std::vector<char> worked(relays.size(), 0);
  {
    std::vector<std::jthread> threads;
    threads.reserve(relays.size());
    for (std::size_t index = 0; index < relays.size(); ++index) {
      threads.emplace_back([&, index] {
        worked[index] = work(index, errors[index]) ? 1 : 0;
      });
    }
  }
  for (std::size_t index = 0; index < relays.size(); ++index) {
    if (worked[index] != 0) {
      errors[index].clear();
    } else if (errors[index].empty()) {
      errors[index] = "failed";
    }
  }
  return errors;
}

// "relay: reason; relay: reason" for every relay that failed.
std::string failures(const std::vector<std::string> &relays,
                     const std::vector<std::string> &errors) {
  std::string out;
  for (std::size_t index = 0; index < relays.size(); ++index) {
    if (!errors[index].empty()) {
      out += (out.empty() ? "" : "; ") + relays[index] + ": " + errors[index];
    }
  }
  return out;
}

} // namespace

NostrDirectory::NostrDirectory(const std::string &game, std::vector<std::string> relays,
                               TlsTrust trust)
    : topic_{"lucent-" + game}, relays_{std::move(relays)}, trust_{std::move(trust)},
      secret_{crypto::random_bytes(crypto::kKeyBytes)} {
  if (!valid_game_name(game)) {
    throw std::invalid_argument{"not a directory game name: " + game};
  }
}

NostrDirectory::~NostrDirectory() {
  crypto::wipe(secret_);
}

bool NostrDirectory::send(const Lobby &lobby, std::int64_t lifetime, std::string &error) {
  const std::int64_t now = now_seconds();
  nostr::Event event;
  event.created_at = now;
  event.kind = kNostrLobbyKind;
  event.tags = {{"d", lobby.id}, {"t", topic_}, {"expiration", std::to_string(now + lifetime)}};
  event.content = lobby_json(lobby, false);
  if (!nostr::sign(event, secret_)) {
    error = "the lobby key does not sign";
    return false;
  }
  const std::vector<std::string> errors =
      each_relay(relays_, [&](std::size_t index, std::string &relay_error) {
        const std::unique_ptr<nostr::Relay> relay =
            nostr::Relay::connect(relays_[index], trust_, relay_error);
        return relay && relay->publish(event, relay_error);
      });
  const std::size_t taken =
      static_cast<std::size_t>(std::count(errors.begin(), errors.end(), std::string{}));
  const std::string failed = failures(relays_, errors);
  if (taken == 0) {
    error = relays_.empty() ? "no relays" : "no relay took the lobby: " + failed;
    return false;
  }
  lucent::log(Level::Info, "nostr",
              std::to_string(taken) + " of " + std::to_string(relays_.size()) +
                  " relays took the lobby" + (failed.empty() ? "" : "; " + failed));
  return true;
}

bool NostrDirectory::publish(const Lobby &lobby, std::string &error) {
  if (!publishable(lobby)) {
    error = "a lobby needs an id and an invite, fields within their limits and seats that add up";
    return false;
  }
  return send(lobby, kNostrLobbySeconds, error);
}

bool NostrDirectory::withdraw(const std::string &id, std::string &error) {
  Lobby empty;
  empty.id = id;
  return send(empty, kWithdrawnSeconds, error);
}

std::optional<std::vector<Lobby>> NostrDirectory::list(std::string &error) {
  const std::int64_t now = now_seconds();
  const std::string filter = "{\"kinds\":[" + std::to_string(kNostrLobbyKind) + "],\"#t\":[" +
                             text::json_quote(topic_) +
                             "],\"since\":" + std::to_string(now - kNostrLobbySeconds) +
                             ",\"limit\":" + std::to_string(kMaxListedLobbies) + "}";
  std::vector<std::vector<nostr::Event>> found(relays_.size());
  const std::vector<std::string> errors =
      each_relay(relays_, [&](std::size_t index, std::string &relay_error) {
        const std::unique_ptr<nostr::Relay> relay =
            nostr::Relay::connect(relays_[index], trust_, relay_error);
        std::optional<std::vector<nostr::Event>> events =
            relay ? relay->query(filter, relay_error) : std::nullopt;
        if (events) {
          found[index] = std::move(*events);
        }
        return events.has_value();
      });
  if (std::count(errors.begin(), errors.end(), std::string{}) == 0) {
    error = relays_.empty() ? "no relays" : "no relay answered: " + failures(relays_, errors);
    return std::nullopt;
  }
  // The newest event per host and lobby id decides, even when it withdraws the lobby.
  std::map<std::pair<std::string, std::string>, const nostr::Event *> newest;
  for (const std::vector<nostr::Event> &events : found) {
    for (const nostr::Event &event : events) {
      const std::optional<std::string_view> id = event.tag("d");
      if (event.kind != kNostrLobbyKind || event.tag("t") != topic_ || !id) {
        continue;
      }
      const nostr::Event *&kept = newest[{event.pubkey, std::string{*id}}];
      if (kept == nullptr || event.created_at > kept->created_at) {
        kept = &event;
      }
    }
  }
  std::vector<std::pair<std::int64_t, Lobby>> open;
  for (const auto &[key, event] : newest) {
    if (std::optional<Lobby> lobby = lobby_of(*event, now)) {
      open.emplace_back(event->created_at, std::move(*lobby));
    }
  }
  std::stable_sort(open.begin(), open.end(), [](const auto &first, const auto &second) {
    return first.first > second.first;
  });
  std::vector<Lobby> lobbies;
  lobbies.reserve(open.size());
  for (auto &[created, lobby] : open) {
    lobbies.push_back(std::move(lobby));
  }
  return lobbies;
}

} // namespace lucent::p2p

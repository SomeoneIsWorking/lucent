#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lucent/p2p/crypto.h"
#include "lucent/p2p/directory.h"
#include "lucent/p2p/tls_stream.h"

namespace lucent::p2p {

// Public relays that store and return an addressable event with an expiry, checked live.
inline constexpr std::array<std::string_view, 5> kDefaultNostrRelays{
    "wss://relay.primal.net", "wss://relay.snort.social", "wss://offchain.pub",
    "wss://nostr.oxtr.dev", "wss://purplerelay.com"};

// A lobby lapses this long after its last publish.
inline constexpr std::int64_t kNostrLobbySeconds = 300;
// NIP-78 application data: addressable, so a republish replaces the last one.
inline constexpr int kNostrLobbyKind = 30078;

// Lobbies as signed Nostr events on several relays at once: one per lobby (the `d` tag), the game
// as a `t` tag, a NIP-40 expiry. Every call reaches the relays in parallel; one that answers is
// enough. The signing key is this object's own, so a host is a fresh key each run.
class NostrDirectory final : public Directory {
public:
  NostrDirectory(std::string game, std::vector<std::string> relays, TlsTrust trust);
  ~NostrDirectory() override;
  NostrDirectory(const NostrDirectory &) = delete;
  NostrDirectory &operator=(const NostrDirectory &) = delete;
  NostrDirectory(NostrDirectory &&) = delete;
  NostrDirectory &operator=(NostrDirectory &&) = delete;

  bool publish(const Lobby &lobby, std::string &error) override;
  std::optional<std::vector<Lobby>> list(std::string &error) override;
  bool withdraw(const std::string &id, std::string &error) override;

private:
  bool send(const Lobby &lobby, std::int64_t lifetime, std::string &error);

  std::string topic_;
  std::vector<std::string> relays_;
  TlsTrust trust_;
  crypto::Bytes secret_;
};

} // namespace lucent::p2p

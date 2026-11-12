#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "lucent/p2p/crypto.h"
#include "lucent/p2p/directory.h"

// A seat in a listed lobby, asked for and given through a directory's mailboxes. The host lists a
// lobby naming a contact key it made for this listing; a player writes an ask to it under a key of
// its own, and the host answers with an invite (or "full") sealed to that key. The invite never
// appears in a listing, and only a player who asks learns where the host is. Every call blocks on
// the directory, so the application makes them off its frame loop.
namespace lucent::p2p {

// Asks one contact takes a minute; the rest wait in the mailbox, or lapse.
inline constexpr std::size_t kMaxAsksPerMinute = 8;

using UnixClock = std::function<std::int64_t()>;

class LobbyHost {
public:
  struct Asker {
    crypto::Bytes key;
  };

  // Lists `lobby` (its `contact` is filled in here) through `directory`, which must outlive this.
  // `now` gives Unix seconds; the system clock unless a test steps one.
  LobbyHost(Directory &directory, Lobby lobby, UnixClock now = {});

  const Lobby &lobby() const noexcept {
    return lobby_;
  }
  // Publishes now if the listing is due (first, or a third of `kLobbySeconds` since), so it never
  // lapses while this host keeps calling. False, with `error`, when the directory took nothing.
  bool keep_listed(std::string &error);
  // Changes the seats taken; published at the next `keep_listed`.
  void seat(int players);
  // Players who asked since the last call: each key once, at most kMaxAsksPerMinute a minute.
  // Nothing, with `error`, when the directory did not answer.
  std::optional<std::vector<Asker>> asks(std::string &error);
  // Seals `invite` (or "full", for none) to `asker`.
  bool answer(const Asker &asker, const std::optional<std::string> &invite, std::string &error);
  // Takes the lobby off the list. A host that lists again makes a new `LobbyHost`, so a new
  // contact key.
  bool withdraw(std::string &error);

private:
  Directory &directory_;
  UnixClock now_;
  crypto::KeyPair contact_;
  Lobby lobby_;
  std::int64_t next_publish_ = 0;
  bool listed_ = false;
  // Askers answered or refused, and letters already read.
  std::set<crypto::Bytes> heard_;
  std::set<crypto::Bytes> read_;
  std::deque<std::int64_t> taken_at_;
};

class LobbyGuest {
public:
  struct Answer {
    // Nothing when the host is full.
    std::optional<std::string> invite;
  };

  // Asks the host of `listed` through `directory`, which must outlive this, under a fresh key.
  LobbyGuest(Directory &directory, Lobby listed);

  // Writes the ask; the host sees it within kLetterSeconds or not at all.
  bool ask(std::string &error);
  // The host's answer once it came; nothing yet (`error` set only when the directory did not
  // answer).
  std::optional<Answer> answer(std::string &error);

private:
  Directory &directory_;
  Lobby listed_;
  crypto::Bytes contact_;
  crypto::KeyPair own_;
};

} // namespace lucent::p2p

#include "lucent/p2p/lobby_seats.h"

#include "lucent/p2p/letter.h"
#include "lucent/text.h"

#include <stdexcept>
#include <utility>

namespace lucent::p2p {
namespace {

constexpr std::string_view kInvited = "invite\n";
constexpr std::string_view kFull = "full";
// Letters remembered as read before the memory starts over; each lapses in kLetterSeconds.
constexpr std::size_t kMaxRemembered = 4096;

crypto::Bytes digest_of(const std::string &letter) {
  return crypto::sha256(crypto::bytes_of(letter));
}

} // namespace

LobbyHost::LobbyHost(Directory &directory, Lobby lobby, UnixClock now)
    : directory_{directory}, now_{now ? std::move(now) : UnixClock{unix_seconds}},
      lobby_{std::move(lobby)} {
  lobby_.contact = mailbox_of(contact_.public_key());
}

bool LobbyHost::keep_listed(std::string &error) {
  const std::int64_t now = now_();
  if (listed_ && now < next_publish_) {
    return true;
  }
  if (!directory_.publish(lobby_, error)) {
    return false;
  }
  listed_ = true;
  next_publish_ = now + kLobbySeconds / 3;
  return true;
}

void LobbyHost::seat(int players) {
  if (players != lobby_.players) {
    lobby_.players = players;
    next_publish_ = 0;
  }
}

std::optional<std::vector<LobbyHost::Asker>> LobbyHost::asks(std::string &error) {
  std::optional<std::vector<std::string>> letters = directory_.letters(lobby_.contact, error);
  if (!letters) {
    return std::nullopt;
  }
  const std::int64_t now = now_();
  while (!taken_at_.empty() && taken_at_.front() <= now - 60) {
    taken_at_.pop_front();
  }
  if (read_.size() > kMaxRemembered) {
    read_.clear();
  }
  std::vector<Asker> found;
  for (const std::string &letter : *letters) {
    if (taken_at_.size() >= kMaxAsksPerMinute) {
      break;
    }
    if (letter.size() > kMaxLetterTextBytes || !read_.insert(digest_of(letter)).second) {
      continue;
    }
    std::optional<OpenedLetter> opened = read_letter(contact_, letter, LetterPurpose::Ask);
    if (!opened || opened->plain != lobby_.id || !heard_.insert(opened->from).second) {
      continue;
    }
    taken_at_.push_back(now);
    found.push_back(Asker{.key = std::move(opened->from)});
  }
  return found;
}

bool LobbyHost::answer(const Asker &asker, const std::optional<std::string> &invite,
                       std::string &error) {
  const std::string plain = invite ? std::string{kInvited} + *invite : std::string{kFull};
  if (plain.size() > kMaxLetterPlainBytes) {
    error = "the invite is longer than a letter holds";
    return false;
  }
  return directory_.post(mailbox_of(asker.key),
                         write_letter(contact_, asker.key, LetterPurpose::Answer, plain), error);
}

bool LobbyHost::withdraw(std::string &error) {
  listed_ = false;
  return directory_.withdraw(lobby_.id, error);
}

LobbyGuest::LobbyGuest(Directory &directory, Lobby listed)
    : directory_{directory}, listed_{std::move(listed)} {
  std::optional<std::vector<std::uint8_t>> contact =
      valid_mailbox(listed_.contact) ? text::from_hex(listed_.contact) : std::nullopt;
  if (!contact) {
    throw std::invalid_argument{"a listed lobby has a contact"};
  }
  contact_ = std::move(*contact);
}

bool LobbyGuest::ask(std::string &error) {
  std::string letter;
  try {
    letter = write_letter(own_, contact_, LetterPurpose::Ask, listed_.id);
  } catch (const std::invalid_argument &) {
    error = "the lobby's contact is not a key";
    return false;
  }
  return directory_.post(listed_.contact, letter, error);
}

std::optional<LobbyGuest::Answer> LobbyGuest::answer(std::string &error) {
  const std::optional<std::vector<std::string>> letters =
      directory_.letters(mailbox_of(own_.public_key()), error);
  if (!letters) {
    return std::nullopt;
  }
  for (const std::string &letter : *letters) {
    std::optional<OpenedLetter> opened = read_letter(own_, letter, LetterPurpose::Answer);
    if (!opened || !crypto::same_bytes(opened->from, contact_)) {
      continue;
    }
    if (opened->plain == kFull) {
      return Answer{};
    }
    if (opened->plain.starts_with(kInvited)) {
      return Answer{.invite = opened->plain.substr(kInvited.size())};
    }
  }
  return std::nullopt;
}

} // namespace lucent::p2p

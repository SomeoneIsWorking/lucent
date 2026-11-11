#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lucent/p2p/crypto.h"
#include "lucent/p2p/path.h"
#include "lucent/udp_socket.h"

// Where one player may be reached, per backend, and the codes two people carry across a chat app
// in place of a server: the host's invite (the descriptor and the meeting's secret) and, when the
// direct paths need it, the friend's reply. Each backend's part is a tagged field, so a reader
// skips a backend it does not know. Crockford base32 after the application's prefix; whitespace,
// dashes and case are ignored on the way in, as a chat app may change them.
namespace lucent::p2p {

// How many of each kind of address a direct reach holds.
inline constexpr std::size_t kMostAddresses = 3;
inline constexpr std::size_t kSecretBytes = 16;

// The direct backend's part: a UDP port and the addresses it may be reached at.
struct DirectReach {
  std::uint16_t port = 0;
  // As seen from the internet: STUN's answer and the router's mapping.
  std::vector<net::Endpoint> publics;
  // This machine's interface addresses, which `port` is bound on.
  std::vector<std::uint32_t> locals;
  // The NAT gives each destination another port, which no other machine can guess.
  bool varying = false;

  // Every address to try.
  std::vector<net::Endpoint> addresses() const;
  bool operator==(const DirectReach &) const = default;
};

// The ZeroTier backend's part: the node, and the port that names its ad-hoc network and the UDP
// port on it.
struct ZeroTierReach {
  // 40 bits.
  std::uint64_t node = 0;
  std::uint16_t port = 0;

  bool operator==(const ZeroTierReach &) const = default;
};

struct Descriptor {
  std::optional<DirectReach> direct;
  std::optional<ZeroTierReach> zerotier;

  bool empty() const noexcept {
    return !direct && !zerotier;
  }
  bool operator==(const Descriptor &) const = default;
};

struct Invite {
  crypto::Bytes secret;
  Descriptor descriptor;
};

// The application's names for its two kinds of code.
struct CodePrefixes {
  std::string_view invite;
  std::string_view reply;
};

// Why a pasted code was not taken. The application words these for its players.
enum class CodeProblem : std::uint8_t {
  Empty,
  // Not this application's code.
  Unknown,
  // An invite where a reply belongs, or the other way round.
  WrongKind,
  // A letter is wrong or missing.
  Damaged,
  // From a newer version.
  Version,
  // A reply, but to another invite.
  OtherInvite,
};

std::string write_invite(const Invite &invite, const CodePrefixes &prefixes);
std::optional<Invite> read_invite(std::string_view text, const CodePrefixes &prefixes,
                                  CodeProblem &problem);

// `descriptor` as a reply to the invite with `secret`.
std::string write_reply(const Descriptor &descriptor, std::span<const std::uint8_t> secret,
                        const CodePrefixes &prefixes);
std::optional<Descriptor> read_reply(std::string_view text, std::span<const std::uint8_t> secret,
                                     const CodePrefixes &prefixes, CodeProblem &problem);

// A code in groups of four, as a screen shows it; the readers take either form.
std::string spaced(std::string_view code);

} // namespace lucent::p2p

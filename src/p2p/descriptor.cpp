#include "lucent/p2p/descriptor.h"

#include <algorithm>
#include <cctype>

#include "wire_bytes.h"

namespace lucent::p2p {
namespace {

constexpr std::string_view kAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr std::uint8_t kVersion = 2;
constexpr std::size_t kCheckBytes = 2;
constexpr std::size_t kTagBytes = 8;
constexpr std::uint8_t kFlagVarying = 1;
constexpr std::size_t kPublicBytes = 6;
constexpr std::size_t kLocalBytes = 4;
constexpr std::size_t kNodeBytes = 5;
constexpr std::size_t kGroupChars = 4;
constexpr std::string_view kReplyLabel = "lucent-p2p\nreply";

using crypto::Bytes;

std::string base32(std::string_view bytes) {
  std::string text;
  text.reserve((bytes.size() * 8 + 4) / 5);
  std::uint32_t held = 0;
  unsigned bits = 0;
  for (const char byte : bytes) {
    held = (held << 8u) | static_cast<std::uint8_t>(byte);
    bits += 8;
    while (bits >= 5) {
      bits -= 5;
      text.push_back(kAlphabet[(held >> bits) & 31u]);
    }
  }
  if (bits > 0) {
    text.push_back(kAlphabet[(held << (5 - bits)) & 31u]);
  }
  return text;
}

// The letters a person may mistake for digits read as those digits.
std::optional<std::uint8_t> value_of(char letter) {
  if (letter == 'O') {
    return std::uint8_t{0};
  }
  if (letter == 'I' || letter == 'L') {
    return std::uint8_t{1};
  }
  const std::size_t at = kAlphabet.find(letter);
  if (at == std::string_view::npos) {
    return std::nullopt;
  }
  return static_cast<std::uint8_t>(at);
}

std::optional<std::string> bytes_of(std::string_view text) {
  std::string bytes;
  bytes.reserve(text.size() * 5 / 8);
  std::uint32_t held = 0;
  unsigned bits = 0;
  for (const char letter : text) {
    const std::optional<std::uint8_t> value = value_of(letter);
    if (!value) {
      return std::nullopt;
    }
    held = (held << 5u) | *value;
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      bytes.push_back(static_cast<char>((held >> bits) & 0xFFu));
    }
  }
  return bytes;
}

// Upper case, without whatever a chat app put between the letters.
std::string cleaned(std::string_view text) {
  std::string clean;
  clean.reserve(text.size());
  for (const char letter : text) {
    const auto byte = static_cast<unsigned char>(letter);
    if (std::isalnum(byte) != 0) {
      clean.push_back(static_cast<char>(std::toupper(byte)));
    }
  }
  return clean;
}

std::string direct_field(const DirectReach &reach) {
  std::string bytes;
  bytes.push_back(static_cast<char>(reach.varying ? kFlagVarying : 0));
  wire::append_u16(bytes, reach.port);
  const std::size_t publics = std::min(reach.publics.size(), kMostAddresses);
  const std::size_t locals = std::min(reach.locals.size(), kMostAddresses);
  bytes.push_back(static_cast<char>((publics << 4u) | locals));
  for (std::size_t at = 0; at < publics; ++at) {
    wire::append_u32(bytes, reach.publics[at].address);
    wire::append_u16(bytes, reach.publics[at].port);
  }
  for (std::size_t at = 0; at < locals; ++at) {
    wire::append_u32(bytes, reach.locals[at]);
  }
  return bytes;
}

std::optional<DirectReach> direct_in(std::string_view bytes) {
  if (bytes.size() < 4) {
    return std::nullopt;
  }
  DirectReach reach;
  reach.varying = (static_cast<std::uint8_t>(bytes[0]) & kFlagVarying) != 0;
  reach.port = wire::u16_at(bytes, 1);
  const std::size_t publics = static_cast<std::uint8_t>(bytes[3]) >> 4u;
  const std::size_t locals = static_cast<std::uint8_t>(bytes[3]) & 15u;
  if (publics > kMostAddresses || locals > kMostAddresses ||
      bytes.size() != 4 + publics * kPublicBytes + locals * kLocalBytes) {
    return std::nullopt;
  }
  std::size_t at = 4;
  for (std::size_t index = 0; index < publics; ++index, at += kPublicBytes) {
    reach.publics.push_back({wire::u32_at(bytes, at), wire::u16_at(bytes, at + 4)});
  }
  for (std::size_t index = 0; index < locals; ++index, at += kLocalBytes) {
    reach.locals.push_back(wire::u32_at(bytes, at));
  }
  return reach;
}

std::string zerotier_field(const ZeroTierReach &reach) {
  std::string bytes;
  for (std::size_t at = 0; at < kNodeBytes; ++at) {
    bytes.push_back(static_cast<char>((reach.node >> (8 * (kNodeBytes - 1 - at))) & 0xFFu));
  }
  wire::append_u16(bytes, reach.port);
  return bytes;
}

std::optional<ZeroTierReach> zerotier_in(std::string_view bytes) {
  if (bytes.size() != kNodeBytes + 2) {
    return std::nullopt;
  }
  ZeroTierReach reach;
  for (std::size_t at = 0; at < kNodeBytes; ++at) {
    reach.node = (reach.node << 8u) | static_cast<std::uint8_t>(bytes[at]);
  }
  reach.port = wire::u16_at(bytes, kNodeBytes);
  return reach;
}

void append_field(std::string &bytes, Backend backend, const std::string &field) {
  bytes.push_back(static_cast<char>(backend));
  bytes.push_back(static_cast<char>(field.size()));
  bytes.append(field);
}

std::string fields_of(const Descriptor &descriptor) {
  std::string bytes;
  if (descriptor.direct) {
    append_field(bytes, Backend::Direct, direct_field(*descriptor.direct));
  }
  if (descriptor.zerotier) {
    append_field(bytes, Backend::ZeroTier, zerotier_field(*descriptor.zerotier));
  }
  return bytes;
}

// Every field `bytes` holds entire; a backend this build does not know is skipped.
std::optional<Descriptor> descriptor_in(std::string_view bytes) {
  Descriptor descriptor;
  for (std::size_t at = 0; at < bytes.size();) {
    if (at + 2 > bytes.size()) {
      return std::nullopt;
    }
    const auto backend = static_cast<std::uint8_t>(bytes[at]);
    const std::size_t length = static_cast<std::uint8_t>(bytes[at + 1]);
    if (at + 2 + length > bytes.size()) {
      return std::nullopt;
    }
    const std::string_view field = bytes.substr(at + 2, length);
    at += 2 + length;
    if (backend == static_cast<std::uint8_t>(Backend::Direct)) {
      descriptor.direct = direct_in(field);
      if (!descriptor.direct) {
        return std::nullopt;
      }
    } else if (backend == static_cast<std::uint8_t>(Backend::ZeroTier)) {
      descriptor.zerotier = zerotier_in(field);
      if (!descriptor.zerotier) {
        return std::nullopt;
      }
    }
  }
  return descriptor;
}

Bytes check_of(std::string_view bytes) {
  Bytes check = crypto::sha256(crypto::bytes_of(bytes));
  check.resize(kCheckBytes);
  return check;
}

Bytes tag_of(std::span<const std::uint8_t> secret, std::string_view bytes) {
  std::string message{kReplyLabel};
  message.append(bytes);
  Bytes tag = crypto::hmac_sha256(secret, crypto::bytes_of(message));
  tag.resize(kTagBytes);
  return tag;
}

// A kind of code's prefix, and the other kind's.
struct Kinds {
  std::string_view own;
  std::string_view other;
};

// What `text` holds after its prefix; nothing, with `problem`, when it starts wrong.
std::optional<std::string> body(std::string_view text, const Kinds &kinds, CodeProblem &problem) {
  const std::string clean = cleaned(text);
  const std::string own_prefix = cleaned(kinds.own);
  const std::string other_prefix = cleaned(kinds.other);
  if (clean.empty()) {
    problem = CodeProblem::Empty;
    return std::nullopt;
  }
  if (clean.starts_with(other_prefix) && !clean.starts_with(own_prefix)) {
    problem = CodeProblem::WrongKind;
    return std::nullopt;
  }
  if (!clean.starts_with(own_prefix)) {
    problem = CodeProblem::Unknown;
    return std::nullopt;
  }
  std::optional<std::string> bytes = bytes_of(std::string_view{clean}.substr(own_prefix.size()));
  if (!bytes || bytes->size() < 2) {
    problem = CodeProblem::Damaged;
    return std::nullopt;
  }
  return bytes;
}

} // namespace

std::vector<net::Endpoint> DirectReach::addresses() const {
  std::vector<net::Endpoint> all = publics;
  for (const std::uint32_t address : locals) {
    all.push_back({address, port});
  }
  return all;
}

std::string write_invite(const Invite &invite, const CodePrefixes &prefixes) {
  std::string bytes(1, static_cast<char>(kVersion));
  bytes.append(crypto::text_of(invite.secret));
  bytes.append(fields_of(invite.descriptor));
  const Bytes check = check_of(bytes);
  bytes.append(crypto::text_of(check));
  return std::string{prefixes.invite} + base32(bytes);
}

std::optional<Invite> read_invite(std::string_view text, const CodePrefixes &prefixes,
                                  CodeProblem &problem) {
  const std::optional<std::string> bytes = body(text, {prefixes.invite, prefixes.reply}, problem);
  if (!bytes) {
    return std::nullopt;
  }
  problem = CodeProblem::Damaged;
  if (bytes->size() < 1 + kSecretBytes + kCheckBytes) {
    return std::nullopt;
  }
  const std::string_view all{*bytes};
  const std::string_view kept = all.substr(0, all.size() - kCheckBytes);
  if (!crypto::same_bytes(check_of(kept), crypto::bytes_of(all.substr(kept.size())))) {
    return std::nullopt;
  }
  if (static_cast<std::uint8_t>(all[0]) != kVersion) {
    problem = CodeProblem::Version;
    return std::nullopt;
  }
  std::optional<Descriptor> descriptor = descriptor_in(kept.substr(1 + kSecretBytes));
  if (!descriptor) {
    return std::nullopt;
  }
  const std::span<const std::uint8_t> secret = crypto::bytes_of(kept.substr(1, kSecretBytes));
  return Invite{Bytes{secret.begin(), secret.end()}, std::move(*descriptor)};
}

std::string write_reply(const Descriptor &descriptor, std::span<const std::uint8_t> secret,
                        const CodePrefixes &prefixes) {
  std::string bytes(1, static_cast<char>(kVersion));
  bytes.append(fields_of(descriptor));
  const Bytes tag = tag_of(secret, bytes);
  bytes.append(crypto::text_of(tag));
  return std::string{prefixes.reply} + base32(bytes);
}

std::optional<Descriptor> read_reply(std::string_view text, std::span<const std::uint8_t> secret,
                                     const CodePrefixes &prefixes, CodeProblem &problem) {
  const std::optional<std::string> bytes = body(text, {prefixes.reply, prefixes.invite}, problem);
  if (!bytes) {
    return std::nullopt;
  }
  problem = CodeProblem::Damaged;
  if (bytes->size() < 1 + kTagBytes) {
    return std::nullopt;
  }
  const std::string_view all{*bytes};
  const std::string_view kept = all.substr(0, all.size() - kTagBytes);
  if (!crypto::same_bytes(tag_of(secret, kept), crypto::bytes_of(all.substr(kept.size())))) {
    problem = CodeProblem::OtherInvite;
    return std::nullopt;
  }
  if (static_cast<std::uint8_t>(all[0]) != kVersion) {
    problem = CodeProblem::Version;
    return std::nullopt;
  }
  return descriptor_in(kept.substr(1));
}

std::string spaced(std::string_view code) {
  std::string text;
  text.reserve(code.size() + code.size() / kGroupChars);
  for (std::size_t at = 0; at < code.size(); ++at) {
    if (at != 0 && at % kGroupChars == 0) {
      text.push_back(' ');
    }
    text.push_back(code[at]);
  }
  return text;
}

} // namespace lucent::p2p

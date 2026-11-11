#include "lucent/p2p/descriptor.h"

#include <cassert>
#include <cctype>
#include <cstdio>
#include <string>

namespace {

using namespace lucent::p2p;

constexpr CodePrefixes kPrefixes{"TST", "TSR"};

Descriptor both() {
  Descriptor descriptor;
  descriptor.direct = DirectReach{40000, {{0x01020304u, 41000}}, {0xC0A80105u, 0x64400001u}, true};
  descriptor.zerotier = ZeroTierReach{0xABCDEF0123u, 9993};
  return descriptor;
}

void invite_round_trips_through_a_chat_app() {
  const Invite invite{crypto::random_bytes(kSecretBytes), both()};
  const std::string code = write_invite(invite, kPrefixes);
  assert(code.starts_with("TST"));
  std::string mangled = "  ";
  for (const char letter : spaced(code)) {
    mangled.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(letter))));
  }
  mangled += "\n";
  CodeProblem problem{};
  const auto read = read_invite(mangled, kPrefixes, problem);
  assert(read && read->secret == invite.secret && read->descriptor == invite.descriptor);
}

void invite_problems_are_named() {
  CodeProblem problem{};
  assert(!read_invite("", kPrefixes, problem) && problem == CodeProblem::Empty);
  assert(!read_invite("XYZ123", kPrefixes, problem) && problem == CodeProblem::Unknown);
  const Invite invite{crypto::random_bytes(kSecretBytes), both()};
  std::string code = write_invite(invite, kPrefixes);
  code[10] = code[10] == '0' ? '2' : '0';
  assert(!read_invite(code, kPrefixes, problem) && problem == CodeProblem::Damaged);
  const std::string reply = write_reply(both(), invite.secret, kPrefixes);
  assert(!read_invite(reply, kPrefixes, problem) && problem == CodeProblem::WrongKind);
}

void reply_is_bound_to_its_invite() {
  const crypto::Bytes secret = crypto::random_bytes(kSecretBytes);
  Descriptor direct_only;
  direct_only.direct = DirectReach{5000, {}, {0x0A000002u}, false};
  const std::string reply = write_reply(direct_only, secret, kPrefixes);
  CodeProblem problem{};
  const auto read = read_reply(reply, secret, kPrefixes, problem);
  assert(read && *read == direct_only);
  assert(!read_reply(reply, crypto::random_bytes(kSecretBytes), kPrefixes, problem) &&
         problem == CodeProblem::OtherInvite);
}

} // namespace

int main() {
  invite_round_trips_through_a_chat_app();
  invite_problems_are_named();
  reply_is_bound_to_its_invite();
  std::puts("lucent p2p descriptor tests passed");
  return 0;
}

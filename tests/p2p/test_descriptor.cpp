#include "lucent/p2p/descriptor.h"

#include <cassert>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

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

Descriptor direct_with(std::vector<std::uint32_t> locals) {
  Descriptor descriptor;
  descriptor.direct =
      DirectReach{40123, {{0x7F000001u, 4000}, {0x7F000002u, 5000}}, std::move(locals), true};
  return descriptor;
}

void a_direct_invite_is_short_enough_to_paste() {
  const Invite invite{crypto::random_bytes(kSecretBytes), direct_with({0xC0A80101u})};
  const std::string code = write_invite(invite, kPrefixes);
  CodeProblem problem{};
  const auto read = read_invite(code, kPrefixes, problem);
  assert(read && read->secret == invite.secret && read->descriptor == invite.descriptor);
  assert(code.size() < 80);
}

void more_refusals_are_named() {
  const Invite invite{crypto::random_bytes(kSecretBytes), direct_with({0xC0A80101u})};
  const std::string code = write_invite(invite, kPrefixes);
  CodeProblem problem{};
  assert(!read_invite("hello there", kPrefixes, problem) && problem == CodeProblem::Unknown);
  std::string changed = code;
  changed[20] = changed[20] == 'A' ? 'B' : 'A';
  assert(!read_invite(changed, kPrefixes, problem) && problem == CodeProblem::Damaged);
  assert(!read_invite(code.substr(0, code.size() - 3), kPrefixes, problem) &&
         problem == CodeProblem::Damaged);
  assert(!read_reply(code, invite.secret, kPrefixes, problem) && problem == CodeProblem::WrongKind);
}

void a_code_holds_what_fits() {
  const Invite invite{crypto::random_bytes(kSecretBytes),
                      direct_with(std::vector<std::uint32_t>(kMostAddresses + 2, 0xC0A80101u))};
  CodeProblem problem{};
  const auto read = read_invite(write_invite(invite, kPrefixes), kPrefixes, problem);
  assert(read && read->descriptor.direct &&
         read->descriptor.direct->locals.size() == kMostAddresses);
}

} // namespace

int main() {
  invite_round_trips_through_a_chat_app();
  invite_problems_are_named();
  reply_is_bound_to_its_invite();
  a_direct_invite_is_short_enough_to_paste();
  more_refusals_are_named();
  a_code_holds_what_fits();
  std::puts("lucent p2p descriptor tests passed");
  return 0;
}

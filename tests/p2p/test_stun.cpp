#include "lucent/p2p/stun.h"

#include <cassert>
#include <cstdio>
#include <string>

namespace {

using namespace lucent::p2p;

void request_and_answer_round_trip() {
  const StunId id = new_stun_id();
  const std::string request = stun_request(id);
  assert(request.size() == 20 && is_stun(request));
  std::string answer = request;
  answer[0] = 0x01;
  answer[1] = 0x01;
  answer[3] = 12;
  // XOR-MAPPED-ADDRESS 203.0.113.7:50000.
  const std::uint16_t port = 50000u ^ 0x2112u;
  const std::uint32_t address = 0xCB007107u ^ 0x2112A442u;
  answer += std::string{"\x00\x20\x00\x08\x00\x01", 6};
  answer.push_back(static_cast<char>(port >> 8u));
  answer.push_back(static_cast<char>(port & 0xFFu));
  for (int shift = 24; shift >= 0; shift -= 8) {
    answer.push_back(static_cast<char>((address >> shift) & 0xFFu));
  }
  const auto mapped = stun_mapped(answer, id);
  assert(mapped && mapped->address == 0xCB007107u && mapped->port == 50000);
  assert(!stun_mapped(answer, new_stun_id()));
  assert(!stun_mapped(answer.substr(0, 19), id));
}

void a_sealed_datagram_is_not_stun() {
  assert(!is_stun(std::string(40, '\x03')));
}

// A Binding success for `id` naming 192.0.2.1:32853, masked as in RFC 5769's sample.
std::string answer_to(const StunId &id) {
  std::string message{"\x01\x01\x00\x0C\x21\x12\xA4\x42", 8};
  for (const std::uint8_t byte : id) {
    message.push_back(static_cast<char>(byte));
  }
  message.append("\x00\x20\x00\x08\x00\x01\xA1\x47\xE1\x12\xA6\x43", 12);
  return message;
}

void the_rfc_5769_address_is_unmasked() {
  const StunId id = new_stun_id();
  const auto mapped = stun_mapped(answer_to(id), id);
  assert(mapped && mapped->address == 0xC0000201u && mapped->port == 32853);
}

void what_is_not_our_answer_is_refused() {
  const StunId id = new_stun_id();
  assert(!stun_mapped(answer_to(new_stun_id()), id));
  // A request is no answer.
  assert(!stun_mapped(stun_request(id), id));
  std::string cut = answer_to(id);
  cut.resize(cut.size() - 4);
  assert(!stun_mapped(cut, id));
  // An attribute longer than the message.
  std::string huge = answer_to(id);
  huge[23] = '\x7F';
  assert(!stun_mapped(huge, id));
  assert(!stun_mapped("", id) && !is_stun("\x03\x0D"));
  assert(is_stun(stun_request(id)));
}

} // namespace

int main() {
  request_and_answer_round_trip();
  a_sealed_datagram_is_not_stun();
  the_rfc_5769_address_is_unmasked();
  what_is_not_our_answer_is_refused();
  std::puts("lucent p2p stun tests passed");
  return 0;
}

#include "lucent/p2p/stun.h"

#include <cassert>
#include <cstdio>

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

} // namespace

int main() {
  request_and_answer_round_trip();
  a_sealed_datagram_is_not_stun();
  std::puts("lucent p2p stun tests passed");
  return 0;
}

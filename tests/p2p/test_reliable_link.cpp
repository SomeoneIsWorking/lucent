#include "lucent/p2p/reliable_link.h"

#include <cassert>
#include <chrono>
#include <cstdio>

#include "lucent/p2p/crypto.h"
#include "loopback_paths.h"

namespace {

using namespace lucent::p2p;
using namespace std::chrono_literals;

struct Pair {
  std::unique_ptr<UdpPath> a_path = test::bound_path();
  std::unique_ptr<UdpPath> b_path = test::bound_path();
  crypto::Bytes a_key = crypto::random_bytes(crypto::kKeyBytes);
  crypto::Bytes b_key = crypto::random_bytes(crypto::kKeyBytes);
};

void a_long_message_arrives_whole_and_in_order() {
  Pair pair;
  LinkCaps caps{};
  caps[2] = 200'000;
  caps[3] = 16;
  ReliableLink a{*pair.a_path, test::loopback_of(*pair.b_path), {pair.a_key, pair.b_key}, caps};
  ReliableLink b{*pair.b_path, test::loopback_of(*pair.a_path), {pair.b_key, pair.a_key}, caps};
  std::string big(150'000, '\0');
  for (std::size_t i = 0; i < big.size(); ++i) {
    big[i] = static_cast<char>(i * 31u);
  }
  a.send(2, big);
  a.send(3, "after");
  std::optional<LinkMessage> first;
  std::optional<LinkMessage> second;
  for (int round = 0; round < 2000 && !second; ++round) {
    a.pump(1ms);
    b.pump(1ms);
    if (auto message = b.take()) {
      (first ? second : first) = std::move(message);
    }
  }
  assert(first && first->kind == 2 && first->body == big);
  assert(second && second->kind == 3 && second->body == "after");
  for (int round = 0; round < 200 && !a.flushed(); ++round) {
    a.pump(1ms);
    b.pump(1ms);
  }
  assert(a.flushed() && a.round_trip());
}

void a_message_past_its_cap_breaks_the_link() {
  Pair pair;
  LinkCaps sender_caps{};
  sender_caps[2] = 1000;
  LinkCaps receiver_caps{};
  receiver_caps[2] = 10;
  ReliableLink a{
      *pair.a_path, test::loopback_of(*pair.b_path), {pair.a_key, pair.b_key}, sender_caps};
  ReliableLink b{
      *pair.b_path, test::loopback_of(*pair.a_path), {pair.b_key, pair.a_key}, receiver_caps};
  a.send(2, std::string(500, 'x'));
  for (int round = 0; round < 100 && !b.broken(); ++round) {
    a.pump(1ms);
    b.pump(1ms);
  }
  assert(b.broken() && !b.take());
}

void datagrams_under_another_key_are_ignored() {
  Pair pair;
  LinkCaps caps{};
  caps[2] = 100;
  ReliableLink a{*pair.a_path, test::loopback_of(*pair.b_path), {pair.a_key, pair.b_key}, caps};
  const crypto::Bytes wrong = crypto::random_bytes(crypto::kKeyBytes);
  ReliableLink b{*pair.b_path, test::loopback_of(*pair.a_path), {pair.b_key, wrong}, caps};
  a.send(2, "secret");
  for (int round = 0; round < 50; ++round) {
    a.pump(1ms);
    b.pump(1ms);
  }
  assert(!b.take() && !b.broken());
}

} // namespace

int main() {
  a_long_message_arrives_whole_and_in_order();
  a_message_past_its_cap_breaks_the_link();
  datagrams_under_another_key_are_ignored();
  std::puts("lucent p2p reliable link tests passed");
  return 0;
}

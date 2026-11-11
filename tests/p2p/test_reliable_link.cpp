#include "lucent/p2p/reliable_link.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>

#include "lucent/p2p/crypto.h"
#include "lucent/p2p/packet_seal.h"
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

constexpr std::uint8_t kAsk = 1;
constexpr std::uint8_t kSave = 2;

LinkCaps rogue_caps() {
  LinkCaps caps{};
  caps[kAsk] = std::size_t{64} * 1024;
  caps[kSave] = std::size_t{16} * 1024 * 1024;
  return caps;
}

void append_le32(std::string &bytes, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    bytes.push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
}

// A data datagram's payload: its kind, a sequence number and a piece of the stream.
std::string data_payload(std::uint32_t sequence, std::string_view chunk) {
  std::string payload(1, '\1');
  append_le32(payload, sequence);
  payload.append(chunk);
  return payload;
}

// A message header in the stream: its kind and size.
struct MessageHead {
  std::uint8_t kind = 0;
  std::uint32_t size = 0;
};

std::string header(const MessageHead &head) {
  std::string bytes(1, static_cast<char>(head.kind));
  append_le32(bytes, head.size);
  return bytes;
}

// A peer that holds the key and sends what no application does.
struct Rogue {
  crypto::Bytes key = crypto::random_bytes(crypto::kKeyBytes);
  crypto::Bytes own = crypto::random_bytes(crypto::kKeyBytes);
  std::unique_ptr<UdpPath> path = test::bound_path();
  std::unique_ptr<UdpPath> target = test::bound_path();
  PacketSeal seal{key};
  ReliableLink link{*target, test::loopback_of(*path), {own, key}, rogue_caps()};

  void send(std::uint32_t sequence, std::string_view chunk) {
    path->send(test::loopback_of(*target),
               seal.seal(ReliableLink::kSealKind, data_payload(sequence, chunk)));
  }
};

void a_message_past_its_cap_breaks_the_link_from_its_header() {
  Rogue rogue;
  rogue.send(0, header({kAsk, 64 * 1024 + 1}));
  rogue.link.pump(50ms);
  assert(rogue.link.broken() && !rogue.link.take());

  Rogue saves;
  saves.send(0, header({kSave, 0xFFFFFFFFu}));
  saves.link.pump(50ms);
  assert(saves.link.broken());
}

void an_unknown_kind_breaks_the_link() {
  Rogue rogue;
  rogue.send(0, header({0x7F, 4}));
  rogue.link.pump(50ms);
  assert(rogue.link.broken());
}

void an_overlong_chunk_breaks_the_link() {
  Rogue rogue;
  rogue.send(0, std::string(ReliableLink::kChunkBytes + 1, '\1'));
  rogue.link.pump(50ms);
  assert(rogue.link.broken());
}

void replays_and_forgeries_are_ignored() {
  Rogue rogue;
  PacketSeal copy{rogue.key};
  const std::string datagram =
      copy.seal(ReliableLink::kSealKind, data_payload(0, header({kAsk, 2}) + "hi"));
  const PathAddress target = test::loopback_of(*rogue.target);
  for (int times = 0; times < 3; ++times) {
    rogue.path->send(target, datagram);
  }
  rogue.path->send(target, std::string(100, 'x'));
  rogue.link.pump(50ms);
  const std::optional<LinkMessage> first = rogue.link.take();
  assert(first && first->kind == kAsk && first->body == "hi");
  assert(!rogue.link.take());
  assert(!rogue.link.broken());
}

void a_far_sequence_is_not_held() {
  Rogue rogue;
  rogue.send(ReliableLink::kWindow + 10, "too far ahead");
  rogue.send(0xFFFFFFFFu, "and further");
  rogue.link.pump(50ms);
  assert(!rogue.link.take() && !rogue.link.broken());
}

} // namespace

int main() {
  a_long_message_arrives_whole_and_in_order();
  a_message_past_its_cap_breaks_the_link();
  datagrams_under_another_key_are_ignored();
  a_message_past_its_cap_breaks_the_link_from_its_header();
  an_unknown_kind_breaks_the_link();
  an_overlong_chunk_breaks_the_link();
  replays_and_forgeries_are_ignored();
  a_far_sequence_is_not_held();
  std::puts("lucent p2p reliable link tests passed");
  return 0;
}

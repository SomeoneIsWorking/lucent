#include "lucent/p2p/reliable_link.h"

#include <algorithm>

#include "wire_bytes.h"

namespace lucent::p2p {
namespace {

using namespace std::chrono_literals;

enum class Datagram : std::uint8_t {
  // A sequence number and a piece of the stream.
  Data = 1,
  // The first sequence number not yet received.
  Ack = 2,
  // The sender's clock in microseconds, to send back.
  Ping = 3,
  Pong = 4,
};

constexpr std::size_t kDataHeader = 1 + 4;
constexpr std::size_t kAckBytes = 1 + 4;
constexpr std::size_t kPingBytes = 1 + 8;
constexpr std::size_t kMessageHeader = 1 + 4;
constexpr auto kResendAfter = 200ms;
constexpr auto kPingEvery = 500ms;
constexpr auto kSilentAfter = 15s;

std::uint64_t microseconds_now() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                        ReliableLink::Clock::now().time_since_epoch())
                                        .count());
}

} // namespace

ReliableLink::ReliableLink(Path &path, PathAddress peer, const LinkKeys &keys, const LinkCaps &caps)
    : path_{path}, peer_{peer}, seal_{keys.send}, opener_{keys.receive}, caps_{caps},
      buffer_(net::kLargestDatagram) {
}

void ReliableLink::send(std::uint8_t kind, std::string_view body) {
  outgoing_.push_back(static_cast<char>(kind));
  wire::append_le<4>(outgoing_, body.size());
  outgoing_.append(body);
}

void ReliableLink::send_sealed(std::string_view payload) {
  path_.send(peer_, seal_.seal(kSealKind, payload));
}

void ReliableLink::send_data(std::uint32_t sequence, std::string_view chunk) {
  std::string payload;
  payload.reserve(kDataHeader + chunk.size());
  payload.push_back(static_cast<char>(Datagram::Data));
  wire::append_le<4>(payload, sequence);
  payload.append(chunk);
  send_sealed(payload);
}

void ReliableLink::send_ack() {
  std::string payload(1, static_cast<char>(Datagram::Ack));
  wire::append_le<4>(payload, expected_);
  send_sealed(payload);
}

void ReliableLink::send_ping(Clock::time_point now) {
  pinged_ = now;
  std::string payload(1, static_cast<char>(Datagram::Ping));
  wire::append_le<8>(payload, microseconds_now());
  send_sealed(payload);
}

void ReliableLink::send_chunks() {
  const Clock::time_point now = Clock::now();
  for (Unacked &waiting : unacked_) {
    if (now - waiting.sent >= kResendAfter) {
      send_data(waiting.sequence, waiting.chunk);
      waiting.sent = now;
    }
  }
  while (unacked_.size() < kWindow && !outgoing_.empty()) {
    const std::size_t size = std::min(kChunkBytes, outgoing_.size());
    Unacked chunk{next_sequence_++, outgoing_.substr(0, size), now};
    outgoing_.erase(0, size);
    send_data(chunk.sequence, chunk.chunk);
    unacked_.push_back(std::move(chunk));
  }
  if (now - pinged_ >= kPingEvery) {
    send_ping(now);
  }
}

void ReliableLink::pump(std::chrono::milliseconds wait) {
  if (broken_) {
    return;
  }
  send_chunks();
  PathAddress from;
  std::optional<std::size_t> length = path_.receive(buffer_, from, wait);
  while (length) {
    if (std::optional<Opened> opened = opener_.open({buffer_.data(), *length});
        opened && opened->kind == kSealKind) {
      peer_ = from;
      heard_ = Clock::now();
      take_datagram(opened->payload);
    }
    length = path_.receive(buffer_, from, 0ms);
  }
}

void ReliableLink::take_datagram(std::string_view payload) {
  if (payload.empty()) {
    return;
  }
  switch (static_cast<Datagram>(payload.front())) {
  case Datagram::Data:
    take_data(payload);
    break;
  case Datagram::Ack:
    take_ack(payload);
    break;
  case Datagram::Ping:
    if (payload.size() == kPingBytes) {
      std::string pong{payload};
      pong.front() = static_cast<char>(Datagram::Pong);
      send_sealed(pong);
    }
    break;
  case Datagram::Pong:
    take_pong(payload);
    break;
  }
}

void ReliableLink::take_data(std::string_view payload) {
  if (payload.size() < kDataHeader || payload.size() > kDataHeader + kChunkBytes) {
    broken_ = true;
    return;
  }
  const auto sequence = static_cast<std::uint32_t>(wire::le_at<4>(payload, 1));
  if (sequence >= expected_ && sequence - expected_ < kWindow) {
    early_.try_emplace(sequence, payload.substr(kDataHeader));
  }
  for (auto next = early_.find(expected_); next != early_.end(); next = early_.find(expected_)) {
    incoming_.append(next->second);
    early_.erase(next);
    ++expected_;
  }
  parse();
  send_ack();
}

void ReliableLink::take_ack(std::string_view payload) {
  if (payload.size() != kAckBytes) {
    return;
  }
  const auto acked = static_cast<std::uint32_t>(wire::le_at<4>(payload, 1));
  while (!unacked_.empty() && unacked_.front().sequence < acked) {
    unacked_.pop_front();
  }
}

void ReliableLink::take_pong(std::string_view payload) {
  if (payload.size() != kPingBytes) {
    return;
  }
  const std::uint64_t sent = wire::le_at<8>(payload, 1);
  const std::uint64_t now = microseconds_now();
  if (sent <= now) {
    round_trip_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::microseconds{now - sent});
  }
}

void ReliableLink::parse() {
  while (!incoming_.empty() && !broken_) {
    const auto kind = static_cast<std::uint8_t>(incoming_.front());
    if (caps_[kind] == 0) {
      broken_ = true;
      return;
    }
    if (incoming_.size() < kMessageHeader) {
      return;
    }
    const std::uint64_t size = wire::le_at<4>(incoming_, 1);
    if (size > caps_[kind]) {
      broken_ = true;
      return;
    }
    if (incoming_.size() < kMessageHeader + size) {
      return;
    }
    arrived_.push_back({kind, incoming_.substr(kMessageHeader, size)});
    incoming_.erase(0, kMessageHeader + size);
  }
}

std::optional<LinkMessage> ReliableLink::take() {
  if (arrived_.empty()) {
    return std::nullopt;
  }
  LinkMessage message = std::move(arrived_.front());
  arrived_.pop_front();
  return message;
}

bool ReliableLink::silent() const noexcept {
  return Clock::now() - heard_ > kSilentAfter;
}

} // namespace lucent::p2p

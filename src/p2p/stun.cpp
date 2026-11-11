#include "lucent/p2p/stun.h"

#include <algorithm>
#include <cstring>

#include "lucent/p2p/crypto.h"
#include "wire_bytes.h"

namespace lucent::p2p {
namespace {

constexpr std::size_t kHeaderBytes = 20;
constexpr std::size_t kIdAt = 8;
constexpr std::size_t kCookieAt = 4;
constexpr std::uint32_t kCookie = 0x2112A442u;
constexpr std::uint16_t kBindingRequest = 0x0001;
constexpr std::uint16_t kBindingSuccess = 0x0101;
constexpr std::uint16_t kMappedAddress = 0x0001;
constexpr std::uint16_t kXorMappedAddress = 0x0020;
constexpr std::uint8_t kFamilyIpv4 = 0x01;
constexpr std::size_t kAddressBytes = 8;

// The address in attribute `value`, masked with the cookie when `masked`.
std::optional<net::Endpoint> address_in(std::string_view value, bool masked) {
  if (value.size() < kAddressBytes || static_cast<std::uint8_t>(value[1]) != kFamilyIpv4) {
    return std::nullopt;
  }
  net::Endpoint endpoint{wire::u32_at(value, 4), wire::u16_at(value, 2)};
  if (masked) {
    endpoint.port = static_cast<std::uint16_t>(endpoint.port ^ (kCookie >> 16u));
    endpoint.address ^= kCookie;
  }
  return endpoint;
}

} // namespace

StunId new_stun_id() {
  const crypto::Bytes random = crypto::random_bytes(kStunIdBytes);
  StunId id{};
  std::copy(random.begin(), random.end(), id.begin());
  return id;
}

std::string stun_request(const StunId &id) {
  std::string message;
  message.reserve(kHeaderBytes);
  wire::append_u16(message, kBindingRequest);
  wire::append_u16(message, 0);
  wire::append_u32(message, kCookie);
  message.append(reinterpret_cast<const char *>(id.data()), id.size());
  return message;
}

bool is_stun(std::string_view datagram) noexcept {
  return datagram.size() >= kHeaderBytes && (static_cast<std::uint8_t>(datagram[0]) & 0xC0u) == 0 &&
         wire::u32_at(datagram, kCookieAt) == kCookie;
}

std::optional<net::Endpoint> stun_mapped(std::string_view datagram, const StunId &id) {
  if (!is_stun(datagram) || wire::u16_at(datagram, 0) != kBindingSuccess ||
      std::memcmp(datagram.data() + kIdAt, id.data(), id.size()) != 0) {
    return std::nullopt;
  }
  const std::size_t end = std::min(datagram.size(), kHeaderBytes + wire::u16_at(datagram, 2));
  std::optional<net::Endpoint> plain;
  for (std::size_t at = kHeaderBytes; at + 4 <= end;) {
    const std::uint16_t type = wire::u16_at(datagram, at);
    const std::size_t length = wire::u16_at(datagram, at + 2);
    if (at + 4 + length > end) {
      break;
    }
    const std::string_view value = datagram.substr(at + 4, length);
    if (type == kXorMappedAddress) {
      return address_in(value, true);
    }
    if (type == kMappedAddress) {
      plain = address_in(value, false);
    }
    at += 4 + ((length + 3) & ~std::size_t{3});
  }
  return plain;
}

} // namespace lucent::p2p

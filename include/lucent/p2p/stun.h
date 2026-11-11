#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "lucent/udp_socket.h"

// The one STUN exchange (RFC 8489) a peer needs: a Binding request, and the address the server saw
// it come from. Nothing else in a STUN message is read.
namespace lucent::p2p {

inline constexpr std::size_t kStunIdBytes = 12;
using StunId = std::array<std::uint8_t, kStunIdBytes>;

StunId new_stun_id();
std::string stun_request(const StunId &id);
// Whether `datagram` is STUN's rather than a sealed one.
bool is_stun(std::string_view datagram) noexcept;
// Where the answer to request `id` says it came from; nothing for anything else.
std::optional<net::Endpoint> stun_mapped(std::string_view datagram, const StunId &id);

} // namespace lucent::p2p

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Fixed-width fields of the wire formats lucent::p2p reads and writes: big-endian for the IETF
// protocols, little-endian for its own. Callers check sizes first.
namespace lucent::p2p::wire {

inline std::uint16_t u16_at(std::string_view bytes, std::size_t at) {
  return static_cast<std::uint16_t>((static_cast<std::uint8_t>(bytes[at]) << 8u) |
                                    static_cast<std::uint8_t>(bytes[at + 1]));
}

inline std::uint32_t u32_at(std::string_view bytes, std::size_t at) {
  return (std::uint32_t{u16_at(bytes, at)} << 16u) | u16_at(bytes, at + 2);
}

inline void append_u16(std::string &bytes, std::uint16_t value) {
  bytes.push_back(static_cast<char>(value >> 8u));
  bytes.push_back(static_cast<char>(value & 0xFFu));
}

inline void append_u32(std::string &bytes, std::uint32_t value) {
  append_u16(bytes, static_cast<std::uint16_t>(value >> 16u));
  append_u16(bytes, static_cast<std::uint16_t>(value & 0xFFFFu));
}

template <std::size_t Bytes> std::uint64_t le_at(std::string_view bytes, std::size_t at) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < Bytes; ++i) {
    value |= std::uint64_t{static_cast<std::uint8_t>(bytes[at + i])} << (8 * i);
  }
  return value;
}

template <std::size_t Bytes> void append_le(std::string &bytes, std::uint64_t value) {
  for (std::size_t i = 0; i < Bytes; ++i) {
    bytes.push_back(static_cast<char>((value >> (8 * i)) & 0xFFu));
  }
}

} // namespace lucent::p2p::wire

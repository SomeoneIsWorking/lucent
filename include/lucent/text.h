#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lucent::text {
namespace detail {

constexpr char lower_ascii_byte(char value) noexcept {
  if (value >= 'A' && value <= 'Z') {
    return static_cast<char>(value + ('a' - 'A'));
  }
  return value;
}

} // namespace detail

// Fold only ASCII letters; non-ASCII bytes are preserved regardless of the process locale.
inline std::string ascii_lower(std::string_view value) {
  std::string result(value);
  for (char &letter : result) {
    letter = detail::lower_ascii_byte(letter);
  }
  return result;
}

inline bool ascii_iequals(std::string_view left, std::string_view right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (detail::lower_ascii_byte(left[index]) != detail::lower_ascii_byte(right[index])) {
      return false;
    }
  }
  return true;
}

// Strip leading and trailing whitespace (std::isspace in the C locale).
std::string_view trim(std::string_view value) noexcept;

// std::from_chars for a decimal real in the C locale, also where the standard library has no
// floating-point from_chars (libc++ before 20, the Android NDK's).
std::from_chars_result from_chars_real(const char *first, const char *last, double &value);
std::from_chars_result from_chars_real(const char *first, const char *last, float &value);

// `value` as a JSON string literal, quotes included, escaped as JSON.stringify escapes it (the
// form a Nostr event id hashes).
std::string json_quote(std::string_view value);

// `bytes` as lowercase hex, two digits each.
std::string hex(std::span<const std::uint8_t> bytes);
// The bytes `digits` spell, either case; nothing for an odd length or a non-hex digit.
std::optional<std::vector<std::uint8_t>> from_hex(std::string_view digits);

// "1 game", "2 games": the count and its English noun, `many` unless the count is one.
std::string counted(std::size_t count, std::string_view one, std::string_view many);
// The noun's plural is `one` with an "s".
std::string counted(std::size_t count, std::string_view one);

} // namespace lucent::text

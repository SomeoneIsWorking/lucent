#pragma once

#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>

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

// `value` as a JSON string literal, quotes included; control bytes are escaped.
std::string json_quote(std::string_view value);

} // namespace lucent::text

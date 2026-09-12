#pragma once

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

} // namespace lucent::text

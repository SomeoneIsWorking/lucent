#include "lucent/text.h"

#include <cctype>

namespace lucent::text {

namespace {
constexpr char kHex[] = "0123456789abcdef";
} // namespace

std::string_view trim(std::string_view value) noexcept {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1);
  }
  return value;
}

std::string json_quote(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('"');
  for (const char c : value) {
    const auto byte = static_cast<unsigned char>(c);
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (byte < 0x20) {
        out += "\\u00";
        out.push_back(kHex[byte >> 4U]);
        out.push_back(kHex[byte & 0x0FU]);
      } else {
        out.push_back(c);
      }
    }
  }
  out.push_back('"');
  return out;
}

} // namespace lucent::text

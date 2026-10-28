#include "lucent/text.h"

#include <cctype>
#include <locale>
#include <sstream>
#include <system_error>

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

namespace {

template <typename Real>
std::from_chars_result parse_real(const char *first, const char *last, Real &value) {
#if defined(__cpp_lib_to_chars)
  return std::from_chars(first, last, value);
#else
  // from_chars accepts no leading space or plus sign; neither does this.
  if (first == last || std::isspace(static_cast<unsigned char>(*first)) != 0 || *first == '+') {
    return {first, std::errc::invalid_argument};
  }
  std::istringstream in{std::string{first, last}};
  in.imbue(std::locale::classic());
  Real parsed{};
  in >> parsed;
  if (in.fail()) {
    return {first, std::errc::invalid_argument};
  }
  const std::streamoff read = in.eof() ? static_cast<std::streamoff>(last - first)
                                       : static_cast<std::streamoff>(in.tellg());
  value = parsed;
  return {first + read, std::errc{}};
#endif
}

} // namespace

std::from_chars_result from_chars_real(const char *first, const char *last, double &value) {
  return parse_real(first, last, value);
}

std::from_chars_result from_chars_real(const char *first, const char *last, float &value) {
  return parse_real(first, last, value);
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

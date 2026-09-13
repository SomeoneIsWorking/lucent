#include "lucent/version.h"

#include <charconv>
#include <cstddef>

namespace lucent::version {
namespace {

// Digits only, no sign, no empty field. std::from_chars would accept a leading
// '-' for an int, and "1.-2.3" must not parse.
bool parse_component(std::string_view text, int &value) {
  if (text.empty()) {
    return false;
  }
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return false;
    }
  }
  int parsed = 0;
  const char *first = text.data();
  const char *last = first + text.size();
  const auto result = std::from_chars(first, last, parsed);
  if (result.ec != std::errc{} || result.ptr != last) {
    return false;
  }
  value = parsed;
  return true;
}

std::optional<std::string_view> quoted_value_after(std::string_view json, std::string_view key) {
  const std::size_t at = json.find(key);
  if (at == std::string_view::npos) {
    return std::nullopt;
  }
  std::size_t cursor = at + key.size();
  while (cursor < json.size() && json[cursor] != ':') {
    ++cursor;
  }
  if (cursor == json.size()) {
    return std::nullopt;
  }
  ++cursor;
  while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t' ||
                                  json[cursor] == '\n' || json[cursor] == '\r')) {
    ++cursor;
  }
  if (cursor == json.size() || json[cursor] != '"') {
    return std::nullopt;
  }
  const std::size_t open = cursor + 1;
  const std::size_t close = json.find('"', open);
  if (close == std::string_view::npos) {
    return std::nullopt;
  }
  return json.substr(open, close - open);
}

} // namespace

std::optional<Triple> parse(std::string_view text) {
  if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
    text.remove_prefix(1);
  }
  const std::size_t first_dot = text.find('.');
  if (first_dot == std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t second_dot = text.find('.', first_dot + 1);
  if (second_dot == std::string_view::npos) {
    return std::nullopt;
  }
  if (text.find('.', second_dot + 1) != std::string_view::npos) {
    return std::nullopt;
  }
  Triple parsed;
  if (!parse_component(text.substr(0, first_dot), parsed.major) ||
      !parse_component(text.substr(first_dot + 1, second_dot - first_dot - 1), parsed.minor) ||
      !parse_component(text.substr(second_dot + 1), parsed.patch)) {
    return std::nullopt;
  }
  return parsed;
}

int compare(const Triple &left, const Triple &right) {
  if (left.major != right.major) {
    return left.major < right.major ? -1 : 1;
  }
  if (left.minor != right.minor) {
    return left.minor < right.minor ? -1 : 1;
  }
  if (left.patch != right.patch) {
    return left.patch < right.patch ? -1 : 1;
  }
  return 0;
}

bool is_newer(std::string_view candidate, std::string_view current) {
  const auto candidate_version = parse(candidate);
  const auto current_version = parse(current);
  if (!candidate_version.has_value() || !current_version.has_value()) {
    return false;
  }
  return compare(*candidate_version, *current_version) > 0;
}

std::optional<std::string> tag_from_release_json(std::string_view json) {
  const auto tag = quoted_value_after(json, "\"tag_name\"");
  if (!tag.has_value()) {
    return std::nullopt;
  }
  return std::string(*tag);
}

} // namespace lucent::version

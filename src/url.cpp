#include "lucent/url.h"

#include <array>
#include <charconv>
#include <utility>

namespace lucent::net {
namespace {

struct Scheme {
  std::string_view name;
  std::uint16_t port;
  bool secure;
};

constexpr std::array<Scheme, 4> kSchemes{
    {{"http", 80, false}, {"https", 443, true}, {"ws", 80, false}, {"wss", 443, true}}};

const Scheme *scheme_named(std::string_view name) {
  for (const Scheme &scheme : kSchemes) {
    if (scheme.name == name) {
      return &scheme;
    }
  }
  return nullptr;
}

} // namespace

bool Url::secure() const noexcept {
  const Scheme *known = scheme_named(scheme);
  return known != nullptr && known->secure;
}

std::string Url::authority() const {
  const Scheme *known = scheme_named(scheme);
  return known != nullptr && known->port == port ? host : host + ":" + std::to_string(port);
}

std::optional<Url> parse_url(std::string_view text) {
  const std::size_t separator = text.find("://");
  if (separator == std::string_view::npos || text.find('#') != std::string_view::npos) {
    return std::nullopt;
  }
  const Scheme *scheme = scheme_named(text.substr(0, separator));
  if (scheme == nullptr) {
    return std::nullopt;
  }
  Url url;
  url.scheme = std::string{scheme->name};
  url.port = scheme->port;
  std::string_view rest = text.substr(separator + 3);
  const std::size_t path = rest.find_first_of("/?");
  if (path != std::string_view::npos) {
    url.target = std::string{rest.substr(path)};
    if (url.target.front() == '?') {
      url.target.insert(url.target.begin(), '/');
    }
    rest = rest.substr(0, path);
  }
  if (rest.find('@') != std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t colon = rest.rfind(':');
  if (colon != std::string_view::npos) {
    const std::string_view digits = rest.substr(colon + 1);
    const auto [end, problem] =
        std::from_chars(digits.data(), digits.data() + digits.size(), url.port);
    if (problem != std::errc{} || end != digits.data() + digits.size() || url.port == 0) {
      return std::nullopt;
    }
    rest = rest.substr(0, colon);
  }
  if (rest.empty()) {
    return std::nullopt;
  }
  url.host = std::string{rest};
  return url;
}

} // namespace lucent::net

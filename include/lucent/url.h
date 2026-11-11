// lucent/url.h — the parts of an http, https, ws or wss URL a client dials.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lucent::net {

struct Url {
  // "http", "https", "ws" or "wss".
  std::string scheme;
  std::string host;
  // The scheme's port when the URL names none.
  std::uint16_t port = 0;
  // The path and query, "/" when the URL has neither.
  std::string target = "/";

  // https and wss: TLS to `host`.
  bool secure() const noexcept;
  // "host" at the scheme's port, "host:port" otherwise, for a Host header.
  std::string authority() const;
};

// Nothing for another scheme, user info, an empty host, a bad port or a fragment.
std::optional<Url> parse_url(std::string_view text);

} // namespace lucent::net

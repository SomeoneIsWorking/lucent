// URLs a client dials: scheme ports, paths and queries, and what is refused.
#include "lucent/url.h"

#include <cassert>
#include <cstdio>

int main() {
  using lucent::net::parse_url;
  const auto relay = parse_url("wss://relay.example");
  assert(relay && relay->secure() && relay->host == "relay.example" && relay->port == 443 &&
         relay->target == "/" && relay->authority() == "relay.example");
  const auto lobby = parse_url("http://127.0.0.1:8080/v1/lobbies?game=lf2");
  assert(lobby && !lobby->secure() && lobby->host == "127.0.0.1" && lobby->port == 8080 &&
         lobby->target == "/v1/lobbies?game=lf2" && lobby->authority() == "127.0.0.1:8080");
  const auto query = parse_url("https://lobby.example:443?x=1");
  assert(query && query->target == "/?x=1" && query->authority() == "lobby.example");
  for (const char *bad : {"ftp://x", "http://", "http://:80", "http://x:0", "http://x:99999",
                          "http://u@x/", "http://x/#top", "relay.example"}) {
    assert(!parse_url(bad));
  }
  std::puts("URLs parse into scheme, host, port and target, and the rest are refused");
  return 0;
}

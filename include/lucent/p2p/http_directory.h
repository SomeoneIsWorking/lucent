#pragma once

#include <string>

#include "lucent/p2p/crypto.h"
#include "lucent/p2p/directory.h"
#include "lucent/p2p/tls_stream.h"
#include "lucent/url.h"

namespace lucent::p2p {

// Lobbies on one self-hosted `LobbyBoard` at `base` (http:// or https://, any path prefix). The
// token is this object's own, so a host is a fresh one each run.
class HttpDirectory final : public Directory {
public:
  // Throws std::invalid_argument for a game name `valid_game_name` refuses or a base that is not an
  // http or https URL.
  HttpDirectory(const std::string &game, const std::string &base, TlsTrust trust);
  ~HttpDirectory() override;
  HttpDirectory(const HttpDirectory &) = delete;
  HttpDirectory &operator=(const HttpDirectory &) = delete;
  HttpDirectory(HttpDirectory &&) = delete;
  HttpDirectory &operator=(HttpDirectory &&) = delete;

  bool publish(const Lobby &lobby, std::string &error) override;
  std::optional<std::vector<Lobby>> list(std::string &error) override;
  bool withdraw(const std::string &id, std::string &error) override;
  bool post(const std::string &mailbox, const std::string &letter, std::string &error) override;
  std::optional<std::vector<std::string>> letters(const std::string &mailbox,
                                                  std::string &error) override;

private:
  // One request to `path` under the base; the body of a 200, or nothing with `error`.
  std::optional<std::string> request(std::string_view method, const std::string &path,
                                     const std::optional<std::string> &body, std::string &error);

  std::string game_;
  net::Url base_;
  TlsTrust trust_;
  crypto::Bytes token_;
};

} // namespace lucent::p2p

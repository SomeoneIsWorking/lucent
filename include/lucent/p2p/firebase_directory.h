#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "lucent/p2p/directory.h"
#include "lucent/p2p/tls_stream.h"
#include "lucent/url.h"

namespace lucent::p2p {

// A Firebase project on the free Spark plan: its Web API key and Realtime Database URL
// (https://<db>.firebaseio.com or https://<db>.<region>.firebasedatabase.app).
struct FirebaseProject {
  std::string api_key;
  std::string database;
  // The Local Emulator Suite instead of Google's servers: the auth emulator's base URL
  // (http://127.0.0.1:9099) and the database namespace, with `database` the database emulator's
  // (http://127.0.0.1:9000).
  std::optional<std::string> auth_emulator;
  std::optional<std::string> emulator_namespace;
};

// Lobbies in a Firebase Realtime Database over REST, each player an anonymous Firebase user. A
// lobby is /lucent/lobbies/<game>/<uid>-<lobby key>, written only by its user (the rules in
// `firebase_rules_json`), listed by expiry, and swept by any player once expired. A letter is
// /lucent/letters/<game>/<mailbox>/<uid>-<random>, written once by its user and swept the same
// way.
class FirebaseDirectory final : public Directory {
public:
  // Throws std::invalid_argument for a game name `valid_game_name` refuses or a URL that is not
  // http or https.
  FirebaseDirectory(const std::string &game, FirebaseProject project, TlsTrust trust);
  ~FirebaseDirectory() override = default;
  FirebaseDirectory(const FirebaseDirectory &) = delete;
  FirebaseDirectory &operator=(const FirebaseDirectory &) = delete;
  FirebaseDirectory(FirebaseDirectory &&) = delete;
  FirebaseDirectory &operator=(FirebaseDirectory &&) = delete;

  bool publish(const Lobby &lobby, std::string &error) override;
  std::optional<std::vector<Lobby>> list(std::string &error) override;
  bool withdraw(const std::string &id, std::string &error) override;
  bool post(const std::string &mailbox, const std::string &letter, std::string &error) override;
  std::optional<std::vector<std::string>> letters(const std::string &mailbox,
                                                  std::string &error) override;

  // This player's Firebase user id once signed in.
  const std::string &user() const noexcept {
    return user_;
  }

private:
  struct Reply {
    int status = 0;
    std::string body;
  };
  // A database location under /lucent/ and the query after `auth`.
  struct Resource {
    std::string path;
    std::string query;
  };

  // Signs in anonymously, or refreshes the ID token a minute before it lapses.
  bool signed_in(std::string &error);
  std::optional<Reply> send(const net::Url &url, std::string_view method,
                            const std::optional<std::string> &body, std::string_view content_type,
                            std::string &error) const;
  // One database request; the body of a 200.
  std::optional<std::string> database(std::string_view method, const Resource &resource,
                                      const std::optional<std::string> &body, std::string &error);
  std::string key_of(const std::string &id) const;
  std::string lobbies() const;
  std::string mailbox_path(const std::string &mailbox) const;
  // Deletes up to a few entries of `collection` that expired before `now`.
  void sweep(const std::string &collection, std::int64_t now);

  std::string game_;
  FirebaseProject project_;
  TlsTrust trust_;
  net::Url database_;
  std::string identity_base_;
  std::string token_base_;
  std::string user_;
  std::string id_token_;
  std::string refresh_token_;
  std::int64_t token_expires_at_ = 0;
};

// The database rules the directory needs, for the console's Rules tab or the emulator.
std::string firebase_rules_json();

} // namespace lucent::p2p

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

#include "lucent/p2p/descriptor.h"
#include "lucent/p2p/direct.h"
#include "lucent/p2p/handshake.h"
#include "lucent/p2p/path.h"
#include "lucent/p2p/port_lease.h"
#include "lucent/p2p/zerotier.h"

// Two people getting from a code to an authenticated path, on a thread of its own: this machine's
// reach gathered, the code shown, then the other machine found by the handshake over each backend
// at once. The direct backend tries the host's addresses, then trades a reply code and punches;
// the ZeroTier one dials the host's node on the ad-hoc network its code names.
// The owner reads `view`, passes on a pasted reply and takes the `Connection` once `Connected`.
// Every wait has a limit.
namespace lucent::p2p {

// How long each wait lasts; the application chooses.
struct Patience {
  // A host's code stays good this long.
  std::chrono::seconds invite{std::chrono::minutes{10}};
  // A friend tries the host alone this long before showing a reply code.
  std::chrono::milliseconds direct{std::chrono::seconds{3}};
  // A friend keeps trying this long after pasting the invite.
  std::chrono::seconds friend_tries{std::chrono::minutes{2}};
  // Both punch this long once the host has the friend's reply.
  std::chrono::seconds punch{std::chrono::seconds{15}};
};

struct ConnectorConfig {
  DirectNetwork direct;
  // The application's node, which outlives the connector; none leaves ZeroTier out.
  const ZeroTierNode *zerotier = nullptr;
  Patience patience;
  CodePrefixes prefixes;
  // Separates this application's keys from another's ("opensu-online").
  std::string label;
};

// What a meeting ends in: the path the two found each other on, kept open for what follows.
struct Connection {
  Role role = Role::Host;
  std::unique_ptr<Path> path;
  PathAddress peer;
  SessionKeys keys;
  // The router's mapping of the path's port, held while the session lasts.
  std::unique_ptr<PortLease> lease;
};

class Connector {
public:
  using Clock = std::chrono::steady_clock;

  enum class Step : std::uint8_t {
    Gathering,
    // The host's code is ready; nobody has arrived.
    Inviting,
    // The friend is trying the host's addresses.
    Trying,
    // The friend's reply code is ready; both keep trying.
    Replying,
    // The host has the reply; both punch.
    Punching,
    // `take` holds the connection.
    Connected,
    Failed,
  };
  enum class Failure : std::uint8_t {
    None,
    // Every backend ran out of ways through.
    Unreachable,
    // The code ran out before anyone arrived.
    Lapsed,
    // No socket, or no address at all.
    NoNetwork,
  };

  struct View {
    Step step = Step::Gathering;
    Failure failure = Failure::None;
    // The code to send: the invite, or the reply.
    std::string code;
    // This side's NAT varies its mapping, so a strict NAT on the other side cannot reach it.
    bool closed = false;
  };

  // A host, with a fresh secret.
  explicit Connector(ConnectorConfig config);
  // A friend who pasted `invite`.
  Connector(ConnectorConfig config, Invite invite);
  ~Connector();
  Connector(const Connector &) = delete;
  Connector &operator=(const Connector &) = delete;
  Connector(Connector &&) = delete;
  Connector &operator=(Connector &&) = delete;

  View view() const;
  // The host's player pasted `code`: nothing when taken, else what is wrong with it.
  std::optional<CodeProblem> reply(std::string_view code);
  // The connection, once, at `Connected`.
  std::unique_ptr<Connection> take();

private:
  void run(const std::stop_token &stop);
  bool find(const std::stop_token &stop, std::unique_ptr<UdpPath> &path, DirectGathered &gathered);
  std::unique_ptr<Path> open_zerotier(std::uint16_t port);
  void connect(std::unique_ptr<Path> path, const Found &found, std::unique_ptr<PortLease> lease);
  void fail(Failure failure);
  void show(Step step);
  void offer(std::string code, bool closed, Step step);
  std::optional<Descriptor> take_reply();

  ConnectorConfig config_;
  Role role_;
  Invite invite_;

  mutable std::mutex mutex_;
  View view_;
  std::optional<Descriptor> reply_;
  std::unique_ptr<Connection> connection_;
  std::jthread worker_;
};

} // namespace lucent::p2p

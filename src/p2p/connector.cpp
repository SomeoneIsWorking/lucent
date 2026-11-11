#include "lucent/p2p/connector.h"

#include <stdexcept>
#include <utility>
#include <vector>

#include "lucent/log.h"
#include "lucent/p2p/stun.h"

namespace lucent::p2p {
namespace {

using namespace std::chrono_literals;

// Keeps the NAT's mapping alive while a code is out.
constexpr auto kStunKeepEvery = 15s;
constexpr auto kStepWait = 50ms;
// A session's ad-hoc network, chosen afresh for each code.
constexpr std::uint16_t kFirstZeroTierPort = 49152;

std::vector<PathAddress> path_addresses(const DirectReach &reach) {
  std::vector<PathAddress> out;
  for (const net::Endpoint &endpoint : reach.addresses()) {
    out.push_back(PathAddress::from(endpoint));
  }
  return out;
}

std::unique_ptr<UdpPath> new_path() {
  std::string error;
  std::optional<net::UdpSocket> socket = net::UdpSocket::bind(0, error);
  if (!socket) {
    throw std::runtime_error{error};
  }
  return std::make_unique<UdpPath>(std::move(*socket));
}

std::uint16_t random_zerotier_port() {
  const crypto::Bytes bytes = crypto::random_bytes(2);
  const auto drawn = static_cast<std::uint16_t>((bytes[0] << 8u) | bytes[1]);
  return static_cast<std::uint16_t>(kFirstZeroTierPort + drawn % (0x10000u - kFirstZeroTierPort));
}

std::string_view text_of(Connector::Failure failure) {
  switch (failure) {
  case Connector::Failure::None:
    return "none";
  case Connector::Failure::Unreachable:
    return "unreachable";
  case Connector::Failure::Lapsed:
    return "the code lapsed";
  case Connector::Failure::NoNetwork:
    return "no network";
  }
  return "unknown";
}

} // namespace

Connector::Connector(ConnectorConfig config)
    : config_{std::move(config)}, role_{Role::Host},
      invite_{crypto::random_bytes(kSecretBytes), {}}, worker_{[this](const std::stop_token &stop) {
        run(stop);
      }} {
}

Connector::Connector(ConnectorConfig config, Invite invite)
    : config_{std::move(config)}, role_{Role::Friend}, invite_{std::move(invite)},
      worker_{[this](const std::stop_token &stop) {
        run(stop);
      }} {
}

Connector::~Connector() {
  worker_.request_stop();
  worker_.join();
  crypto::wipe(invite_.secret);
}

Connector::View Connector::view() const {
  const std::lock_guard lock{mutex_};
  return view_;
}

std::optional<CodeProblem> Connector::reply(std::string_view code) {
  CodeProblem problem{};
  std::optional<Descriptor> descriptor =
      read_reply(code, invite_.secret, config_.prefixes, problem);
  if (!descriptor) {
    return problem;
  }
  const std::lock_guard lock{mutex_};
  reply_ = std::move(descriptor);
  return std::nullopt;
}

std::unique_ptr<Connection> Connector::take() {
  const std::lock_guard lock{mutex_};
  return std::move(connection_);
}

std::optional<Descriptor> Connector::take_reply() {
  const std::lock_guard lock{mutex_};
  return std::exchange(reply_, std::nullopt);
}

void Connector::fail(Failure failure) {
  const std::lock_guard lock{mutex_};
  view_.step = Step::Failed;
  view_.failure = failure;
  view_.code.clear();
  lucent::log(Level::Info, "p2p", "no connection: " + std::string{text_of(failure)});
}

void Connector::show(Step step) {
  const std::lock_guard lock{mutex_};
  view_.step = step;
}

void Connector::offer(std::string code, bool closed, Step step) {
  const std::lock_guard lock{mutex_};
  view_.step = step;
  view_.code = std::move(code);
  view_.closed = closed;
}

void Connector::run(const std::stop_token &stop) {
  try {
    std::unique_ptr<UdpPath> path = new_path();
    DirectGathered gathered = gather_direct(stop, path->socket(), config_.direct);
    if (gathered.reach.publics.empty() && gathered.reach.locals.empty() &&
        config_.zerotier == nullptr) {
      fail(Failure::NoNetwork);
      return;
    }
    find(stop, path, gathered);
  } catch (const std::runtime_error &failure) {
    lucent::log(Level::Error, "p2p", std::string{"the connector cannot go on: "} + failure.what());
    fail(Failure::NoNetwork);
  }
}

std::unique_ptr<Path> Connector::open_zerotier(std::uint16_t port) {
  std::string error;
  std::unique_ptr<Path> path = config_.zerotier->open(port, error);
  if (!path) {
    lucent::log(Level::Warn, "p2p", "no ZeroTier path: " + error);
  }
  return path;
}

void Connector::connect(std::unique_ptr<Path> path, const Found &found,
                        std::unique_ptr<PortLease> lease) {
  const Backend backend = path->backend();
  auto connection = std::make_unique<Connection>(
      Connection{role_, std::move(path), found.peer, found.keys, std::move(lease)});
  const std::lock_guard lock{mutex_};
  connection_ = std::move(connection);
  view_.step = Step::Connected;
  view_.code.clear();
  lucent::log(Level::Info, "p2p",
              backend == Backend::Direct ? "connected directly" : "connected over ZeroTier");
}

bool Connector::find(const std::stop_token &stop, std::unique_ptr<UdpPath> &path,
                     DirectGathered &gathered) {
  const bool host = role_ == Role::Host;
  auto handshake = std::make_unique<Handshake>(*path, role_, invite_.secret, config_.label);
  std::unique_ptr<Path> zerotier_path;
  std::optional<PathAddress> zerotier_peer;
  if (config_.zerotier != nullptr) {
    if (host) {
      zerotier_path = open_zerotier(random_zerotier_port());
    } else if (const std::optional<ZeroTierReach> &reach = invite_.descriptor.zerotier) {
      zerotier_path = open_zerotier(reach->port);
      zerotier_peer = zerotier_address(reach->node, reach->port);
    }
  }
  std::unique_ptr<Handshake> zerotier;
  if (zerotier_path) {
    zerotier = std::make_unique<Handshake>(*zerotier_path, role_, invite_.secret, config_.label);
  }
  const Clock::time_point started = Clock::now();
  Clock::time_point until =
      started + (host ? config_.patience.invite : config_.patience.friend_tries);
  auto next_keep = started + kStunKeepEvery;
  bool replying = false;
  bool punching = false;
  if (host) {
    invite_.descriptor.direct = gathered.reach;
    if (zerotier_path) {
      invite_.descriptor.zerotier = ZeroTierReach{config_.zerotier->id(), zerotier_path->port()};
    }
    offer(write_invite(invite_, config_.prefixes), gathered.reach.varying, Step::Inviting);
  } else if (invite_.descriptor.direct || zerotier) {
    if (invite_.descriptor.direct) {
      handshake->aim(path_addresses(*invite_.descriptor.direct));
    }
    if (zerotier && zerotier_peer) {
      zerotier->aim({*zerotier_peer});
    }
    show(Step::Trying);
  } else {
    fail(Failure::Unreachable);
    return false;
  }
  while (!stop.stop_requested()) {
    handshake->step(kStepWait);
    if (const std::optional<Found> found = handshake->found()) {
      handshake.reset();
      zerotier.reset();
      connect(std::move(path), *found, std::move(gathered.lease));
      return true;
    }
    if (zerotier) {
      zerotier->step(0ms);
      if (const std::optional<Found> found = zerotier->found()) {
        zerotier.reset();
        handshake.reset();
        connect(std::move(zerotier_path), *found, nullptr);
        return true;
      }
    }
    const Clock::time_point now = Clock::now();
    if (gathered.stun_server && now >= next_keep) {
      next_keep = now + kStunKeepEvery;
      path->socket().send(*gathered.stun_server, stun_request(new_stun_id()));
    }
    if (host) {
      if (const std::optional<Descriptor> reply = take_reply(); reply && reply->direct) {
        handshake->aim(path_addresses(*reply->direct));
        punching = true;
        until = now + config_.patience.punch;
        show(Step::Punching);
        lucent::log(Level::Info, "p2p", "punching toward the friend's reply");
      }
    } else if (!replying && invite_.descriptor.direct && now - started >= config_.patience.direct) {
      if (gathered.reach.varying && invite_.descriptor.direct->varying) {
        if (!zerotier) {
          fail(Failure::Unreachable);
          return false;
        }
        // Neither NAT keeps a mapping a punch can use; ZeroTier is the way left.
        replying = true;
        continue;
      }
      replying = true;
      // The direct try reached the host's NAT, so its entry there must not match this port.
      handshake.reset();
      gathered = {};
      path = new_path();
      gathered = gather_direct(stop, path->socket(), config_.direct);
      handshake = std::make_unique<Handshake>(*path, role_, invite_.secret, config_.label);
      handshake->aim(path_addresses(*invite_.descriptor.direct));
      handshake->keep_near();
      Descriptor own;
      own.direct = gathered.reach;
      offer(write_reply(own, invite_.secret, config_.prefixes), gathered.reach.varying,
            Step::Replying);
    }
    if (now >= until) {
      fail(host && !punching ? Failure::Lapsed : Failure::Unreachable);
      return false;
    }
  }
  return false;
}

} // namespace lucent::p2p

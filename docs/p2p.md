# Peer-to-peer transport (`lucent::online`)

Two players connect from a code, with no server of ours and nothing to install. The optional
`lucent::online` target (`LUCENT_BUILD_ONLINE`, OpenSSL libcrypto and libssl) owns it, with the embedded ZeroTier
backend in `lucent::zerotier` (`LUCENT_BUILD_ZEROTIER`, libzt) for a game that opts in; namespace `lucent::p2p`;
the core keeps the title-neutral socket pieces. The plan and its phases are
`docs/plans/0006-online-framework.md`.

## Owns

| Owner | What |
|---|---|
| `net::UdpSocket` (core, `udp_socket.h`) | one IPv4 UDP socket on every interface, bounded waits, hop-limited sends |
| `net::Endpoint`, `parse_ipv4`, `resolve_endpoint`, `private_ipv4` (core, `ipv4.h`) | IPv4 addresses in host byte order |
| `net::default_gateway` (core, `network_interfaces.h`) | the default route's gateway |
| `http::fetch` (core, `http_client.h`) | one plain-HTTP request to a loopback or LAN host (UPnP routers) |
| `crypto` | random bytes, SHA-256, HMAC, HKDF, ChaCha20-Poly1305, X25519, BIP-340 Schnorr |
| `PacketSeal`, `PacketOpener` | one sealed datagram per sender key, counter nonce, replay window |
| `Path`, `UdpPath` | a backend's unauthenticated datagram socket |
| `Descriptor`, invite and reply codes | where a player may be reached, per backend, as a code |
| `stun`, `NatPmp`, `UpnpIgd`, `PortLease` | the direct backend's NAT view and router mapping |
| `gather_direct`, `reachable_locals` | the direct backend's reach |
| `Handshake`, `SessionKeys` | the one authenticated meeting over any path |
| `ReliableLink` | ordered, capped messages over a sealed path |
| `PathBridge` | a local process's datagrams over a path only this process can open |
| `nostr::Event`, `nostr::Relay` | NIP-01 events (canonical form, id, BIP-340 signature) and one ws/wss relay: publish to OK, query to EOSE, forged events dropped |
| `Directory`, `Lobby` | where a game lists open lobbies; it only carries the invite a host would share |
| `LobbyBoard`, `HttpDirectory`, `lucent_lobby_server` | the self-hosted lobby list: the board as a `lucent::http::Server` handler, its client, and the executable that serves it |
| `NostrDirectory` | lobbies as NIP-78 addressable events (`d` per lobby, `t` = `lucent-<game>`, NIP-40 expiry) on every relay at once |
| `TlsStream`, `TlsTrust`, `system_roots_pem`, `connect_stream` | TLS 1.2+ over TCP (OpenSSL libssl) for the HTTP and WebSocket clients, checked against the system's roots and the name dialled; `connect_stream` picks TLS or TCP from a `net::Url` |
| `ZeroTierPaths` | the seam the connector reaches ZeroTier through, if the application gives it one |
| `ZeroTierNode` (`lucent::zerotier`) | the process's one embedded libzt node on the application's ad-hoc network |
| `Connector` | invite → (reply) → found, every backend at once, on its own thread |

The application keeps its message kinds and caps, its wording, its patience and its UI.

## Data flow

1. The host's `Connector` binds a UDP port, gathers its reach (`gather_direct`: interface addresses
   by `reachable_locals`, two STUN servers, NAT-PMP then UPnP) and shows an invite: the descriptor
   and a 16-byte secret, base32 after the application's prefix.
2. The friend pastes it and tries the host's addresses for `Patience::direct`. With no answer it
   binds a new port, gathers again, shows a reply code and sends only 3 hops out, so its NAT opens
   and nothing reaches the host's router first. The host pastes the reply and knocks.
3. `Handshake`: Hello and Welcome carry ephemeral X25519 keys sealed under HKDF(secret, label). The
   friend is connected on the Welcome; the host when the first datagram under the session keys
   opens. `SessionKeys::key(purpose, sender)` derives one key per purpose and sender.
4. With `ZeroTierPaths` in the config and the node on its network, the host also opens a path
   there and puts its node id and the network's port in the invite; the friend, already on the
   same network, dials the host's 6PLANE address there. Both handshakes run together and the first
   to find the other wins. A friend whose direct way is closed (both NATs vary) keeps trying
   ZeroTier instead of failing.
5. `take()` hands over the `Connection`: the open path, the peer, the keys and the router lease. The
   application runs its `ReliableLink` or its game session on it. A session in another process
   (a game runner) cannot open a ZeroTier path, so the application keeps the path in a
   `PathBridge` for the game: the runner sends to `local()` on loopback and the bridge carries it to
   the peer and back. Only loopback senders reach the peer and only the peer reaches the runner.

## Discovery

A host with an invite may list it. `NostrDirectory` signs each lobby with a key of its own (fresh
each run) as kind 30078 with tags `d` (the lobby id), `t` (`lucent-<game>`) and `expiration` (now +
300 s); a republish replaces it, so the host publishes again inside that window while the lobby is
open. Withdrawing publishes the same `d` with no invite and a 60 s expiry. `list` asks every relay
for the game's `t` since 300 s ago, keeps the newest event per host and id, and drops lobbies that
are withdrawn, lapsed, malformed or fail their id or signature. Every call reaches the relays on
threads at once and one relay that answers is enough; the log line names the ones that did not.
`kDefaultNostrRelays` are relays shown to store and return such an event; an application can pass
its own. Nothing about a lobby is trusted beyond its invite: a code from a stranger only connects
to that stranger. `Lobby` fields and their limits, `publishable` and the JSON form
(`lobby_json`, `lobby_of_json`) are one codec in `directory.h` for both directories.

`lucent_lobby_server [--port N]` (default 8077) serves a `LobbyBoard` on every IPv4 interface until
SIGINT or SIGTERM; TLS, when wanted, is a reverse proxy in front. A game points an `HttpDirectory`
at its base URL instead of, or beside, the relays. The board keys a lobby by its host, the SHA-256
of a token only that host's directory holds, so nobody else replaces or withdraws it; a lobby lapses
300 s after its last publish, and the board holds at most 500 lobbies per game and 256 games.

| Request | Body | Answer |
|---|---|---|
| `PUT /v1/lobbies` | `{"game","token","lobby":{id,title,invite,players,capacity,version}}` | `{"expires_at"}` |
| `POST /v1/lobbies/withdraw` | `{"game","token","id"}` | `{}` |
| `GET /v1/lobbies?game=G` | | `{"lobbies":[{...,"host","expires_at"}]}`, newest first |

## ZeroTier

libzt (fork `SomeoneIsWorking/libzt` at `6e74b65`, ZeroTierOne from `SomeoneIsWorking/ZeroTierOne`
branch `lucent` at `b5ef81a3`) is installed into a prefix outside the repo and found as
`libzt::zt`; the configure error names the commands. One node per process: libzt refuses a second,
and `zts_node_free` would end it for the process, so the node's destructor only stops it. Each
application has one ad-hoc network, named by its `ZeroTierConfig::network_port`; the node joins it
as soon as it is online. The application starts the node when its online menu opens
(`ZeroTierNode::start`, identity kept in `ZeroTierConfig::storage`) and hands the connector a
pointer: on the lab, a node is on its network 5 to 15 s after starting, and a friend whose node is
already there connects about 2 s after pasting the code.

- libzt binds no interface whose name starts with `lo`, `zt`, `tun` or `tap` (also `feth` and
  `utun` on macOS), so a node on a machine with only such interfaces (slirp4netns' `tap0`) reaches
  nobody. Tests give their namespace a dummy interface of another name.
- The static `libzt::zt` gives its users `ZTS_STATIC`, so on Windows they call it directly instead
  of through `dllimport`.
- An ad-hoc network (`ff` + start port + end port) has no controller: it carries IPv6 and only UDP
  and TCP to its port range, and each member's address is the 6PLANE one from network and node id.
  It is open to anyone who picks the same port, so the handshake's secret is what keeps it ours.
- `zts_addr_compute_6plane` fills only the address; the path sets the family and length itself, or
  lwIP refuses the send as a bad argument.
- libzt joins a network only once the node is online, so the node joins from a thread of its own.
  A path opened earlier sends nothing until the node has its address; the handshake resends.
- The network's configuration follows from its id, so the node caches none; it keeps its identity
  and its peers. The identity's secret stays in storage and is never logged.
- ZeroTier binds each address to its own interface (`SO_BINDTODEVICE`) and none on loopback, so a
  node takes only datagrams that arrive on the interface holding the address they are sent to; a
  lab root needs its address on the link its peers reach it by. A lab root is any node
  whose identity is in a root set signed with `zts_util_sign_root_set` at a non-loopback address;
  `ZeroTierConfig::roots` takes that set instead of ZeroTier's public roots. `make_root` writes such
  an identity into a node's storage and returns the set.
- A host whose node is not on its network yet waits up to `Patience::zerotier` for it before
  showing the code; a friend's path opens at once and its handshake resends until the node is.

## Descriptor wire

Version 2. Invite: version, secret, fields, 2-byte SHA-256 check. Reply: version, fields, 8-byte
HMAC-SHA-256 tag under the secret. A field is backend id, length, bytes, and a reader skips an id it
does not know. Direct (id 0): flags (bit 0 varying), port, publics/locals counts, then the
addresses. ZeroTier (id 1): 40-bit node id, port.

## Invariants

- Nothing but a STUN answer is read before it opens under a key from the code's secret, and a STUN
  answer must match its transaction id and come from the server asked.
- The `label` separates one application's keys from another's; a code for one never opens another.
- No code, key, public or peer address is logged; `gather_direct` logs this machine's local
  addresses in the order it offers them, so a player can see which network leads.
- A late STUN answer is ignored, and anything else that is not the handshake's counts as dropped.
- Every wait has a limit and the connector stops on destruction.
- A router mapping is asked for 120 s and renewed at half its lease; more than 600 s, or a permanent
  one, is refused.
- A relay's events count only when their id hashes their canonical form and their signature checks
  out; a lobby past its expiry or without an invite is never listed.
- A TLS stream verifies the peer's chain and the dialled name (SNI and host check) before it carries
  a byte; the roots are OpenSSL's default paths on Linux, the CA folders on Android, the Keychain's
  anchors on macOS and the ROOT store on Windows, plus any PEM the caller adds.

## Checking it

`ctest --test-dir build/agent -R p2p` runs the owners through real loopback sockets: forged,
replayed, wrong-secret and other-label handshakes are dropped, NAT-PMP and UPnP stand-ins grant,
renew and take down a lease, a reliable link carries a 150 kB message in order and
breaks on a size past its cap, a fake STUN server answers `gather_direct`, two connectors meet
from a pasted invite, and a TLS stream reaches a loopback server whose certificate a test CA signs
for one name while another name or an untrusted root is refused. Nostr events sign and read
back, a relay stand-in refuses one and answers a query with a forged event among real ones, and a
`NostrDirectory` over two stand-ins and a dead relay lists a lobby once and drops withdrawn, lapsed,
forged and other games' ones. A `LobbyBoard` on a loopback server with a stepped clock lists,
withdraws (only for its host) and lapses lobbies for two `HttpDirectory`s and refuses malformed
requests. Live, `lucent_lobby_server` on this machine's LAN address serves a host and a friend
(2026-11-11). Live, on the default relays: publish reaches all five in about 1.5 s
and a second key lists the lobby with its fields, then none after withdraw (2026-11-11). `lucent_p2p_zerotier_tests` (Linux) runs in its own network namespace with a
dummy interface: a lab root, a host, a friend and a stranger, one process each. Host and friend
connect by a code over ZeroTier alone and trade a message on the reliable link; the stranger, who
has the host's node and network but not the secret, gets nowhere. It prints how long each node took
to reach its network and how long the warm friend took to connect. Across NATs it is proved in openSU's `tools.nat_lab` (rootless namespaces,
`nft` NATs, a STUN server and a NAT-PMP/UPnP gateway).

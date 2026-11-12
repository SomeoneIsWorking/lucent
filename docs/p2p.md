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
| `VirtualLan` | a LAN game's UDP to and from up to 16 friends, each shown at one 10.254.0.x address, sealed over its own connection |
| `nostr::Event`, `nostr::Relay` | NIP-01 events (canonical form, id, BIP-340 signature) and one ws/wss relay: publish to OK, query to EOSE, forged events dropped |
| `Directory`, `Lobby` | where a game lists open lobbies (a contact key, never an invite or an address) and keeps mailboxes of sealed letters |
| `write_letter`, `read_letter` | a short message sealed from one X25519 key to another for one purpose |
| `LobbyHost`, `LobbyGuest` | a seat asked for and given through a directory: the ask sealed to the lobby's contact, the invite sealed back to the asker |
| `LobbyBoard`, `HttpDirectory`, `lucent_lobby_server` | the self-hosted lobby list: the board as a `lucent::http::Server` handler, its client, and the executable that serves it |
| `FirebaseDirectory`, `firebase_rules_json` | lobbies in a Firebase Realtime Database over REST, each player an anonymous user, and the rules that guard them |
| `nostr::RelayStore`, `lucent_nostr_relay` | a lab's own relay: NIP-01 EVENT and REQ, every event's id and signature checked, addressable replace, NIP-40 expiry |
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
6. A game with its own LAN netcode and many friends (X-Men Legends II) gives every `Connection` to
   one `VirtualLan`. The application numbers the session (the host hands out 10.254.0.2 upward, so
   every machine sees a friend at one address) or takes the next free address. The LAN owns each
   connection and seals under `kLinkPurpose`, with a keepalive each way every second: the friend's
   first one completes the host's meeting while the game is silent, and later ones hold NAT
   mappings open. The game's datagram to a friend's address goes out with both ports ahead of the
   bytes; a friend's datagram arrives at the game's port on 127.0.0.1 from a loopback
   socket kept per friend and source port, and `source_of` turns that socket's port back into the
   friend's address for the game's hooks. A datagram is taken only from the friend's peer and only
   under its keys.

## Discovery

A listing is public, so it holds no invite and no address: only the lobby's id, its typed title,
seats, the game version and a contact, the mailbox of an X25519 key the host made for this
listing. Seats go through letters:

1. `LobbyHost` makes the contact key, lists the lobby and publishes it again a third of
   `kLobbySeconds` (300 s) after each publish, so a crashed host's lobby is gone within 300 s.
2. A player who picks it makes `LobbyGuest`, with a key of its own, and posts an ask (the lobby id)
   to the contact's mailbox.
3. `LobbyHost::asks` reads the mailbox and opens each new letter: at most `kMaxAsksPerMinute` (8)
   asks a minute, each asker's key once; letters past `kMaxLetterTextBytes` are dropped before any
   decoding or crypto. The application answers each with its own invite, or "full".
4. The answer is sealed to the asker's key and posted to its mailbox; `LobbyGuest::answer` takes it
   only from the contact key, and the player connects by that invite as by a pasted code.

Only someone who asks learns the invite, and so the host's addresses: a public lobby's address is
public to anyone who asks, and the application says so before listing. Withdrawing takes the
listing down; listing again makes a new `LobbyHost`, so a new contact.

A letter (`letter.h`) is hex of version 1, the sender's public key, a 12-byte random nonce and
ChaCha20-Poly1305 of the text. The key is HKDF-SHA256 of the two keys' X25519 secret, salted with
the nonce, with info `lucent letter v1\n<ask|answer>\n<sender><recipient>`; the version and both
keys are the associated data. An ask does not open as an answer, and nobody but the two key holders
opens either. A letter holds at most 3072 bytes of text and lapses after `kLetterSeconds` (120 s)
in every directory; a mailbox lists at most `kMaxLettersListed` (64).

`NostrDirectory` signs each lobby with a key of its own (fresh each run) as kind 30078 with tags `d`
(the lobby id), `t` (`lucent-<game>`) and `expiration` (now + 300 s); a republish replaces it.
Withdrawing publishes the same `d` with no contact and a 60 s expiry. `list` asks every relay for
the game's `t` since 300 s ago, keeps the newest event per host and id, and drops lobbies that are
withdrawn, lapsed, malformed or fail their id or signature. A letter is a kind 30078 event with a
random `d`, `t` = `lucent-<game>-<mailbox>` and a 120 s expiry, signed by a key made for that
letter alone. Events longer than their kind's limit are dropped before their id or signature is
checked. Every call reaches the relays on threads at once and one relay that answers is enough;
the log line names the ones that did not. `kDefaultNostrRelays` are relays shown to store and
return such an event; an application can pass its own. `Lobby` fields and their limits,
`publishable` and the JSON form (`lobby_json`, `lobby_of_json`), `kLobbySeconds` and
`unix_seconds` are one codec in `directory.h` for every directory.

`lucent_nostr_relay [--port N] [--lan]` (default 7447, loopback unless `--lan`) is a relay for a
lab with no internet, like `lucent_zerotier_root`: it refuses an event whose id is not the SHA-256
of its canonical form or whose signature does not check out, one past its expiry and one over
64 kB, keeps the newest addressable event per author, kind and `d`, and answers REQ filters with
`ids`, `authors`, `kinds`, `#<letter>`, `since`, `until` and `limit`, newest first. Both it and
`lucent_lobby_server` are targets in a consumer's build too, so a game's lab builds them in its own
tree.

`lucent_lobby_server [--port N]` (default 8077) serves a `LobbyBoard` on every IPv4 interface until
`StopSignals` hears a stop request; TLS, when wanted, is a reverse proxy in front. A game points an `HttpDirectory`
at its base URL instead of, or beside, the relays. The board keys a lobby by its host, the SHA-256
of a token only that host's directory holds, so nobody else replaces or withdraws it; a lobby lapses
300 s after its last publish, and the board holds at most 500 lobbies per game and 256 games.
A mailbox takes 16 letters a minute (429 past that) and keeps its newest 64; a game holds at most
4096 mailboxes.

| Request | Body | Answer |
|---|---|---|
| `PUT /v1/lobbies` | `{"game","token","lobby":{id,title,contact,players,capacity,version}}` | `{"expires_at"}` |
| `POST /v1/lobbies/withdraw` | `{"game","token","id"}` | `{}` |
| `GET /v1/lobbies?game=G` | | `{"lobbies":[{...,"host","expires_at"}]}`, newest first |
| `POST /v1/letters` | `{"game","mailbox","letter"}` | `{}` |
| `GET /v1/letters?game=G&mailbox=M` | | `{"letters":[...]}`, newest first |

`FirebaseDirectory` keeps lobbies in a Firebase Realtime Database on the free Spark plan, over REST
with no SDK. Each player signs in anonymously (Identity Toolkit `accounts:signUp`, the token
service refreshing the ID token a minute before it lapses) and writes
`/lucent/lobbies/<game>/<uid>-<32 hex of sha256(id)>` with its `host` and `expires_at`. The rules
from `firebase_rules_json()` let only that user write the key, any signed-in player delete an
expired one, cap every field and the expiry at 600 s ahead, and index `expires_at`. `list` asks
for lobbies expiring after now (`orderBy="expires_at"&startAt`, at most 200) and keeps only those
whose key starts with their `host`; each publish deletes up to 8 expired lobbies. A letter is
`/lucent/letters/<game>/<mailbox>/<uid>-<32 random hex>` with `letter` and `expires_at`: written
once and only by that user, readable by any signed-in player, expiry capped at 240 s ahead, and up
to 8 expired ones deleted by each post to the mailbox. Firebase's rules cannot count, so the rate
cap is the host's own. `FirebaseProject`
names the Web API key and database URL, or the Local Emulator Suite's auth URL and namespace.

## ZeroTier

libzt (fork `SomeoneIsWorking/libzt` at `9932062`, ZeroTierOne from `SomeoneIsWorking/ZeroTierOne`
branch `lucent` at `23c01391`) is installed into a prefix outside the repo and found as
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
  an identity into a node's storage and returns the set. `lucent_zerotier_root <storage>
  <address/port> <roots file>` (every platform but Android) is that root as a program for a
  game's lab: it writes the set to the file and runs until `StopSignals` hears a stop request.
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
  out; a lobby past its expiry or without a contact is never listed.
- No listing holds an invite or an address; a letter opens only for its two keys and purpose, and
  a host hears each asker's key once.
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
forged and other games' ones, and keeps letters to their mailbox, dropping an oversized one. A
`LobbyBoard` on a loopback server with a stepped clock lists, withdraws (only for its host) and
lapses lobbies for two `HttpDirectory`s, keeps letters to their mailbox, lapses and rate-caps them,
and refuses malformed requests. `lucent_p2p_letter_tests` opens letters only for their recipient
and purpose; `lucent_p2p_crypto_tests` runs the RFC 7748 §6.1 X25519, RFC 8439 §2.8.2
ChaCha20-Poly1305 and RFC 5869 HKDF vectors. `lucent_p2p_lobby_seats_tests` seats players through a
loopback board: the invite reaches the asker alone, each key is heard once, the ninth ask in a
minute waits, "full" arrives, and an impostor's answer is not taken. The Firebase test runs the
same against the Local Emulator Suite with `firebase_rules_json()` (2026-11-12).
`lucent_p2p_nostr_relay_tests` runs the lab relay on loopback: it takes an event signed by
rust-nostr and refuses forged, re-signed, expired and oversized ones, replaces an addressable
event, and a Nostr directory lists, withdraws and seats a player through it.
`lucent_p2p_nostr_tests` checks that same foreign event's id and signature, and
`lucent_p2p_schnorr_tests` runs BIP-340's own vectors. Open: lobbies and letters on a live public
relay since the contact change; the live run of 2026-11-11 below predates it. Live, `lucent_lobby_server` on this machine's LAN address serves a host and a friend
(2026-11-11). Live, on the default relays: publish reaches all five in about 1.5 s
and a second key lists the lobby with its fields, then none after withdraw (2026-11-11). `lucent_p2p_zerotier_tests` (Linux) runs in its own network namespace with a
dummy interface: a lab root, then a host, a friend and a stranger, each a `lucent_p2p_zerotier_peer`.
Host and friend connect by a code over ZeroTier alone and trade a message on the reliable link; the
stranger, who has the host's node and network but not the secret, gets nowhere. It prints how long
each node took to reach its network and how long the warm friend took to connect. The peer is
portable: `--friend-runner wine --friend-peer <peer.exe>` seats a Windows build (llvm-mingw, static
OpenSSL and libzt) as the friend against Linux host and root processes. Across NATs it is proved in openSU's `tools.nat_lab` (rootless namespaces,
`nft` NATs, a STUN server and a NAT-PMP/UPnP gateway).

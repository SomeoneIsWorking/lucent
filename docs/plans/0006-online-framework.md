---
id: 0006
title: Online framework — transport, discovery and rollback for every multiplayer game
status: in-progress
state_items: [S015, S016, S017, S018, S019]
created: 2026-11-11
updated: 2026-11-11
---

# Online framework

## The request

- "how can we integrate zerotier support for my multiplayer games ... OpenSU, Little Fighter,
  X-Men, whatever else that is multiplayer capable, like Crash Bash and others"
- "last time I tried to use zero tier, I couldn't set it up ... maybe you can do something where the
  setup is handled automatically"
- "Lf2 would probably need a new rollback netcode ... and anything that doesn't have its own
  multiplayer ofc also gets rollback netcode"
- "Firebase for you know a fixed discovery point online"
- "Make the framework capable of multiple solutions imo, Incase firebase doesn't work out, but all
  solutions should require no extra install like a VPN client like tailscale"

Every backend runs inside the game process: no install, root, VPN permission or player account, on
desktop or Android. Browser builds wait for a WebRTC backend.

## Design

One optional target, `lucent::online` (`LUCENT_BUILD_ONLINE`, OpenSSL libcrypto), namespace
`lucent::p2p`. The dependency-free core gains only what is not online-specific: the IPv4 UDP socket
and the default gateway beside `network_interfaces`.

### Transport

| Owner | Does |
|---|---|
| `crypto` | random bytes, SHA-256, HMAC, HKDF, ChaCha20-Poly1305, X25519 on libcrypto |
| `packet_seal` | one datagram sealed under a per-sender key, counter as nonce, replay window |
| `path` | an unauthenticated datagram pipe to one peer on one backend, possibly to several candidate addresses until one is proved |
| `descriptor` | a player's candidates per backend (TLV, unknown backend skipped), written as a code with the app's prefixes: invite (secret + check) and reply (HMAC under the secret) |
| `stun`, `nat_pmp`, `upnp_igd`, `port_mapper`, `port_lease` | the direct backend's view of its own NAT, moved from openSU |
| `direct` | UDP backend: gather (socket, STUN, router mapping, local addresses) and a `Path` that punches |
| `zerotier` | libzt backend: ad-hoc network `ff` + ports + `000000`, persisted identity, node id as the code, peer address computed offline |
| `handshake` | one authenticated meeting over any `Path`: sealed Hello/Welcome/Knock under HKDF(secret), X25519, per-direction session keys; the code is spent on the first datagram under the session keys |
| `reliable_link` | ordered capped messages over a sealed `Path` |
| `connector` | tries backends in order, each with its own patience, through invite → (reply) → found; ends in a `Path` plus session keys or a named failure |

The application keeps its payloads (openSU's Ask/Save/Go), its UI and its patience values. The
process that plays owns the path: a runner links `lucent::online` and reaches the agreed backend
itself.

### Playing by code

A shared code needs no discovery at all: the invite is the serialised descriptor (direct candidates
and the ZeroTier node behind one secret). The friend tries the direct paths, then ZeroTier, before
`Unreachable`. Every game stays playable by code with every directory off or unreachable.

### Discovery

Optional, and only publishes the same descriptor. `Directory` interface: `publish(entry)`,
`list(game)`, `withdraw()`. A game queries several directories and merges; a directory that is down
degrades to the others.

- Nostr first and default: a short-lived signed event per lobby (NIP-78 kind 30078, addressable,
  so a republish replaces it) with a NIP-40 expiry, sent to a shipped list of 5 public relays; readers query them all and merge. A ZeroTier node id instead of
  an IP where it will do. Prior art first (Trystero's Nostr signalling; libsecp256k1, MIT). Relays
  are WebSocket over TLS: needs a TLS client (OpenSSL libssl; Android trust store through the shared
  android-port layer).
- A self-hosted lobby on `lucent::http::Server` second: the same interface, a config change away,
  for when the user rents a server. Proved locally.
- Firebase third: Realtime Database over REST with Anonymous Auth (Identity Toolkit `signUp`), on
  the free Spark plan, behind the same interface; rules let only a lobby's owner write it and index
  its expiry. Proved on the Firebase Local Emulator Suite first; the user's own project needs
  console steps on their Google account. EOS stays out (its terms rule out fan ports).

### Rollback

One title-neutral engine whose messages the application carries over a sealed link: input
exchange, prediction, rewind and resimulate, input delay, final-hash desync detection, spectating
if cheap. The runtime supplies save state, load state, set the pads, advance one frame (8 slots of
1 byte for the first consumers) and a state hash. GGPO (MIT) is the design reference.

## Phases

1. Transport interface and direct backend, moved from openSU; openSU consumes it; `tools.nat_lab`
   green before and after. **In progress.**
2. Embedded ZeroTier on the SomeoneIsWorking libzt fork, shared prefix, `find_package`; Android NDK
   arm64-v8a and x86_64.
3. Nostr discovery. **Shown in Lucent** (`NostrDirectory`, live on the five default relays); a game's
   lobby screen consuming it is next.
4. Self-hosted lobby. **Shown in Lucent** (`LobbyBoard`, `HttpDirectory`, `lucent_lobby_server`, live
   on a LAN address); the user rents a server when the relays are not enough.
5. Rollback engine with psxport (Crash Bash 4-player), then LF2. **Shown in LF2** (two players
   rewinding over netem and over ZeroTier with equal final hashes, lf2 816f7ec); psxport's four
   players are next.
6. X-Men Legends II: game's own LAN netcode over the transport; broadcast discovery becomes unicast to
   friend codes and directory entries. `VirtualLan` shows each friend at one 10.254.0.x address.

Each phase is done when two or more processes in rootless network namespaces play in the consuming
product. ZeroTier is proved by openSU's `tools.nat_lab` with symmetric NATs on both sides, so that
only the ZeroTier path can connect.

## Consumers

- openSU (`src/online`): direct transport, then ZeroTier when the direct paths fail.
- psxport (plan online-netplay, S025): rollback's first consumer.
- LF2 (plan 0074): rollback's second consumer and the first lobby.
- X-Men Legends II (S023): transport and directory under the game's own netcode.

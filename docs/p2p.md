# Peer-to-peer transport (`lucent::online`)

Two players connect from a code, with no server of ours and nothing to install. The optional
`lucent::online` target (`LUCENT_BUILD_ONLINE`, OpenSSL libcrypto) owns it, namespace `lucent::p2p`;
the core keeps the title-neutral socket pieces. The plan and its phases are
`docs/plans/0006-online-framework.md`.

## Owns

| Owner | What |
|---|---|
| `net::UdpSocket` (core, `udp_socket.h`) | one IPv4 UDP socket on every interface, bounded waits, hop-limited sends |
| `net::Endpoint`, `parse_ipv4`, `resolve_endpoint`, `private_ipv4` (core, `ipv4.h`) | IPv4 addresses in host byte order |
| `net::default_gateway` (core, `network_interfaces.h`) | the default route's gateway |
| `http::fetch` (core, `http_client.h`) | one plain-HTTP request to a loopback or LAN host (UPnP routers) |
| `crypto` | random bytes, SHA-256, HMAC, HKDF, ChaCha20-Poly1305, X25519 |
| `PacketSeal`, `PacketOpener` | one sealed datagram per sender key, counter nonce, replay window |
| `Path`, `UdpPath` | a backend's unauthenticated datagram socket |
| `Descriptor`, invite and reply codes | where a player may be reached, per backend, as a code |
| `stun`, `NatPmp`, `UpnpIgd`, `PortLease` | the direct backend's NAT view and router mapping |
| `gather_direct`, `reachable_locals` | the direct backend's reach |
| `Handshake`, `SessionKeys` | the one authenticated meeting over any path |
| `ReliableLink` | ordered, capped messages over a sealed path |
| `Connector` | invite → (reply) → found, each backend in turn, on its own thread |

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
4. `take()` hands over the `Connection`: the open path, the peer, the keys and the router lease. The
   application runs its `ReliableLink` or its game session on it.

## Descriptor wire

Version 2. Invite: version, secret, fields, 2-byte SHA-256 check. Reply: version, fields, 8-byte
HMAC-SHA-256 tag under the secret. A field is backend id, length, bytes, and a reader skips an id it
does not know. Direct (id 0): flags (bit 0 varying), port, publics/locals counts, then the
addresses. ZeroTier (id 1): 40-bit node id, port.

## Invariants

- Nothing but a STUN answer is read before it opens under a key from the code's secret, and a STUN
  answer must match its transaction id and come from the server asked.
- The `label` separates one application's keys from another's; a code for one never opens another.
- No code, key or address is logged.
- Every wait has a limit and the connector stops on destruction.
- A router mapping is asked for 120 s and renewed at half its lease; more than 600 s, or a permanent
  one, is refused.

## Checking it

`ctest --test-dir build/agent -R p2p` runs the owners through real loopback sockets: forged, wrong
-secret and other-label handshakes are dropped, a reliable link carries a 150 kB message in order and
breaks on a size past its cap, a fake STUN server answers `gather_direct`, and two connectors meet
from a pasted invite. Across NATs it is proved in openSU's `tools.nat_lab` (rootless namespaces,
`nft` NATs, a STUN server and a NAT-PMP/UPnP gateway).

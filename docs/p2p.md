# Peer-to-peer transport (`lucent::online`)

Two players connect from a code, with no server of ours and nothing to install. The optional
`lucent::online` target (`LUCENT_BUILD_ONLINE`, OpenSSL libcrypto and libzt) owns it, namespace `lucent::p2p`;
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
| `ZeroTierNode`, `zerotier_address` | the process's one embedded libzt node and a path per ad-hoc network |
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
4. With a `ZeroTierNode` in the config, the host also opens a path on the ad-hoc network of a fresh
   port (49152 and up) and puts its node id and that port in the invite; the friend joins that
   network and dials the host's 6PLANE address there. Both handshakes run together and the first
   to find the other wins. A friend whose direct way is closed (both NATs vary) keeps trying
   ZeroTier instead of failing.
5. `take()` hands over the `Connection`: the open path, the peer, the keys and the router lease. The
   application runs its `ReliableLink` or its game session on it.

## ZeroTier

libzt (fork `SomeoneIsWorking/libzt` at `7c8378c`, ZeroTierOne from `SomeoneIsWorking/ZeroTierOne`
branch `lucent` at `588f476d`) is installed into a prefix outside the repo and found as
`libzt::zt`; the configure error names the commands. One node per process: libzt refuses a second,
and `zts_node_free` would end it for the process, so the node's destructor only stops it. The
application starts it early (`ZeroTierNode::start`, identity kept in `ZeroTierConfig::storage`),
since reaching the roots takes seconds, and hands the connector a pointer.

- An ad-hoc network (`ff` + start port + end port) has no controller: it carries IPv6 and only UDP
  and TCP to its port range, and each member's address is the 6PLANE one from network and node id.
  It is open to anyone who picks the same port, so the handshake's secret is what keeps it ours.
- `zts_addr_compute_6plane` fills only the address; the path sets the family and length itself, or
  lwIP refuses the send as a bad argument.
- A path sends nothing until the node has its address on the network, a few seconds after joining;
  the handshake's resends cover it.
- ZeroTier binds every interface but loopback and sends nothing over it. A lab root is any node
  whose identity is in a root set signed with `zts_util_sign_root_set` at a non-loopback address;
  `ZeroTierConfig::roots` takes that set instead of ZeroTier's public roots.

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

## Checking it

`ctest --test-dir build/agent -R p2p` runs the owners through real loopback sockets: forged,
replayed, wrong-secret and other-label handshakes are dropped, NAT-PMP and UPnP stand-ins grant,
renew and take down a lease, a reliable link carries a 150 kB message in order and
breaks on a size past its cap, a fake STUN server answers `gather_direct`, and two connectors meet
from a pasted invite. `lucent_p2p_zerotier_tests` (Linux) runs in its own network namespace with a
dummy interface: a lab root, a host and a friend, one process each, connect by a code over ZeroTier
alone and trade a message on the reliable link. Across NATs it is proved in openSU's `tools.nat_lab` (rootless namespaces,
`nft` NATs, a STUN server and a NAT-PMP/UPnP gateway).

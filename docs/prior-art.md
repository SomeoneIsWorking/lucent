# Prior art and third-party code

What Lucent takes from others, under which licence, at which revision.

## Peer-to-peer transport (`lucent::online`, plan 0006)

| Work | Licence at our pin | What we take |
|---|---|---|
| openSU `src/online` (ours, MIT) | MIT | The direct backend, handshake, reliable link and codes, moved here and made title-neutral (Lucent 56a58ed) |
| OpenSSL 3 libcrypto | Apache-2.0 | Random bytes, SHA-256, HMAC, HKDF, ChaCha20-Poly1305, X25519, linked from the system or the consumer's prefix |
| zerotier/libzt, fork `SomeoneIsWorking/libzt`, upstream `a707ea6` | BSL 1.1 until its Change Date 2026-01-01, Apache-2.0 since (`LICENSE.txt`) | The embedded ZeroTier node and its socket API (phase 2); fork changes are commits there |
| ZeroTierOne at `c53c6bd9c3` (libzt's `ext/ZeroTierOne`) | BSL 1.1 until 2025-01-01, Apache-2.0 since | The ZeroTier core libzt builds |
| lwIP `32708c0a8b` and lwip-contrib `4fd612c9c7` | BSD-3-Clause | libzt's user-space IP stack |
| miniupnpc and libnatpmp (in ZeroTierOne `ext/`) | BSD-3-Clause | ZeroTier's own router port mapping |
| concurrentqueue (libzt `ext/`) | Simplified BSD | libzt's event queue |
| prometheus-cpp-lite (ZeroTierOne `ext/`) | MIT | Headers ZeroTierOne includes |

A BSL revision is pinned only once its Change Date has passed; recheck `LICENSE.txt` of libzt and
`ext/ZeroTierOne/LICENSE.txt` at every pin bump.

Still to check before they are built: GGPO and its maintained forks (MIT) for rollback; Trystero's
Nostr signalling and libsecp256k1 (MIT) for Nostr discovery; libdatachannel (MPL-2.0) for WebRTC.

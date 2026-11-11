# Lucent guidance

Lucent owns small C++20 infrastructure that several projects would otherwise reimplement. Its core
is dependency-free; optional `lucent::zip` adds zlib-backed archive extraction and optional Linux
`lucent::file_dialog` adds GTK3/GIO.

- `config` owns typed, cached environment configuration.
- `cvar` owns layered configuration variables: a compiled default overridden by a `name = value`
  file, then the environment (via `config`) or an explicit `--set`, then a runtime set. Consumers define `Var<T>`
  globals and register them; C code reads them through `cvar_c.h`. Persistence policy (which file,
  where) stays with the consumer.
- `log` owns all process diagnostics and sinks.
- `perf` (`thread_work_counter`, `work_clock`, `count_histogram`) owns load-independent cost
  units: a thread's user-space instructions retired (thread CPU time where no counter opens), their
  sum across threads that hand off, and constant-memory count percentiles. Consumers own phases,
  report lines and what they meter; the method is re-harness's `perf-units` skill.
- `http` owns bounded loopback transport, request parsing, response framing, and server lifecycle.
  Consumers own routes and domain behavior; game input, renderer probes, and application state do
  not belong in Lucent.
- `network_interfaces` owns host IPv4 interface enumeration (address, netmask, broadcast,
  flags), the default gateway, and the rule for which broadcast addresses reach every LAN.
  `udp_socket` and `ipv4` own the IPv4 UDP socket and address type.
- `online` (optional, `LUCENT_BUILD_ONLINE`, OpenSSL libcrypto and libssl; on by default only in
  Lucent's own build) owns peer-to-peer transport in `lucent::p2p`: codes, NAT traversal, the
  authenticated handshake, the reliable link, TLS, and lobby directories on Nostr relays or a
  self-hosted `lucent_lobby_server` (`docs/p2p.md`, plan `docs/plans/0006-online-framework.md`).
  Consumers own message kinds and caps, wording, patience and UI.
- `touch` owns platform-neutral contact-to-zone capture, multi-touch routing, and cancellation.
  Android/SDL3 shells own event acquisition, safe-area layout, and conversion from zone IDs to game
  actions; do not put title-specific controls in Lucent.
- `platform` owns portable per-application user-data directory resolution and private directory
  creation. Its Android consumer provides the app-private root through the C ABI; the separate shared android-port framework owns persisted SAF
  read grants plus bounded private staging and promotion; consumers own setup wording, install
  validation, archive policy, and the decision to promote after validation.
- `content` owns dependency-free streaming content digests. Consumers own the expected identity and
  complete-install policy.
- `file_dialog` owns one asynchronous Linux native file selection and its cancellation lifetime.
  `file_read` separately owns bounded GIO regular-file reads. Consumers poll these main-thread
  owners and retain file interpretation, validation, selection wording, and publication policy.
- `file_store` owns asynchronous immutable backing files under a caller-supplied trusted parent,
  aggregate byte/count budgets, private child ownership, and terminal cleanup after consumers have
  released their native file handles. It does not own media formats or player policy.
- `zip` owns safe ZIP entry discovery, bounded extraction, and exactly-one candidate selection for
  user-provided install archives. Consumers supply the destination and title-specific filename or
  content-identity matcher; game-specific archive layouts and validation do not belong in Lucent.

Full quality check: `python3 tools/check_cpp_quality.py build`. Tests: `ctest --test-dir build`.
`zerotier` (optional, `LUCENT_BUILD_ZEROTIER`, needs `online`; on by default only in Lucent's own
build) is the embedded ZeroTier backend a game links to opt in.

Focused online tests: `ctest --test-dir build/agent -R p2p`. The zerotier target finds libzt (revision
in `docs/p2p.md`) through `CMAKE_PREFIX_PATH`; the agent tree uses
`~/.local/share/lucent-thirdparty/libzt-<revision>-<compiler>`. The ZeroTier test needs `unshare` and `ip`.

# Lucent guidance

The repository-wide rules in `../AGENTS.md` apply here. Lucent owns small C++20 infrastructure
that several projects would otherwise reimplement; its core remains dependency-free and optional
`lucent::zip` adds zlib-backed archive extraction.

- `config` owns typed, cached environment configuration.
- `log` owns all process diagnostics and sinks.
- `http` owns bounded loopback transport, request parsing, response framing, and server lifecycle.
  Consumers own routes and domain behavior; game input, renderer probes, and application state do
  not belong in Lucent.
- `touch` owns platform-neutral contact-to-zone capture, multi-touch routing, and cancellation.
  Android/SDL3 shells own event acquisition, safe-area layout, and conversion from zone IDs to game
  actions; do not put title-specific controls in Lucent.
- `platform` owns portable per-application user-data directory resolution and private directory
  creation. Android shells provide their app-private root through the C ABI; they own URI selection
  and document copying, while consumers own install validation and archive policy.
- `zip` owns safe ZIP entry discovery and extraction for user-provided install archives. Consumers
  supply the required filename and destination; game-specific archive layouts do not belong in
  Lucent.

Run `tools/check_cpp_quality.sh scratch/build` and `ctest --test-dir scratch/build` before landing.
Update `docs/codemap.md` in the same commit when ownership changes.

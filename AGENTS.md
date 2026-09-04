# Lucent guidance

The repository-wide rules in `../AGENTS.md` apply here. Lucent owns small C++20 infrastructure
that several projects would otherwise reimplement; its core remains dependency-free and optional
`lucent::zip` adds zlib-backed archive extraction.

- `config` owns typed, cached environment configuration.
- `cvar` owns layered configuration variables: a compiled default overridden by a `name = value`
  file, then the environment (via `config`), then an explicit `--set`. Consumers define `Var<T>`
  globals and register them; C code reads them through `cvar_c.h`. Persistence policy (which file,
  where) stays with the consumer.
- `log` owns all process diagnostics and sinks.
- `http` owns bounded loopback transport, request parsing, response framing, and server lifecycle.
  Consumers own routes and domain behavior; game input, renderer probes, and application state do
  not belong in Lucent.
- `touch` owns platform-neutral contact-to-zone capture, multi-touch routing, and cancellation.
  Android/SDL3 shells own event acquisition, safe-area layout, and conversion from zone IDs to game
  actions; do not put title-specific controls in Lucent.
- `platform` owns portable per-application user-data directory resolution and private directory
  creation. Its Android shell provides the app-private root through the C ABI and owns persisted SAF
  read grants plus bounded private staging and promotion; consumers own setup wording, install
  validation, archive policy, and the decision to promote after validation.
- `content` owns dependency-free streaming content digests. Consumers own the expected identity and
  complete-install policy.
- `zip` owns safe ZIP entry discovery, bounded extraction, and exactly-one candidate selection for
  user-provided install archives. Consumers supply the destination and title-specific filename or
  content-identity matcher; game-specific archive layouts and validation do not belong in Lucent.

Run `python3 tools/check_cpp_quality.py build` and `ctest --test-dir build` before landing.
Update `docs/codemap.md` in the same commit when ownership changes.

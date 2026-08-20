# Lucent guidance

The repository-wide rules in `../AGENTS.md` apply here. Lucent owns dependency-free C++20
infrastructure that several projects would otherwise reimplement.

- `config` owns typed, cached environment configuration.
- `log` owns all process diagnostics and sinks.
- `http` owns bounded loopback transport, request parsing, response framing, and server lifecycle.
  Consumers own routes and domain behavior; game input, renderer probes, and application state do
  not belong in Lucent.

Run `tools/check_cpp_quality.sh scratch/build` and `ctest --test-dir scratch/build` before landing.
Update `docs/codemap.md` in the same commit when ownership changes.

# Codemap

Lucent is a dependency-free C++20 infrastructure library. Each public header has one cohesive
implementation owner; consumers compose the pieces and retain all application-specific policy.
Clang is the verification toolchain; compatible GCC and AppleClang consumers are accepted.

| Subsystem | Status | Where | Gap/next |
|---|---|---|---|
| Configuration | real, tested | `include/lucent/config.h`, `src/config.cpp` | Cached environment configuration only; no file format |
| Logging | real, tested | `include/lucent/log.h`, `src/log.cpp` | Thread-safe text sinks; every stderr, file, and installed-sink record carries a millisecond UTC ISO 8601 timestamp |
| Loopback HTTP | real, tested | `include/lucent/http.h`, `src/http.cpp` | One request per connection; no TLS, remote binding, keep-alive, or chunked transfer by design |
| Verification | real | `tests/`, `tools/check_cpp_quality.sh`, `tools/check_source_structure.py` | Run through CTest in top-level builds |

## Source tree

```text
include/lucent/  public configuration, logging, and HTTP interfaces
src/             one implementation translation unit per public subsystem
tests/           production-interface unit and loopback integration tests
tools/           non-mutating quality and structure gates
```

## Where is X?

- Read a typed environment value: `lucent::config` in `include/lucent/config.h`.
- Emit or capture a diagnostic: `lucent::log` in `include/lucent/log.h`.
- Add a local interactive control channel: `lucent::http::Server` in `include/lucent/http.h`; the
  consumer supplies the route handler.

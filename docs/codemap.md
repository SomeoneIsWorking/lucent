# Codemap

Lucent is a dependency-free C++20 infrastructure library. Each public header has one cohesive
implementation owner; consumers compose the pieces and retain all application-specific policy.
Clang is the verification toolchain; compatible GCC and AppleClang consumers are accepted.

| Subsystem | Status | Where | Gap/next |
|---|---|---|---|
| Configuration | real, tested | `include/lucent/config.h`, `src/config.cpp` | Cached environment configuration only; no file format |
| Logging | real, tested | `include/lucent/log.h`, `src/log.cpp` | Thread-safe text sinks; every stderr, file, and installed-sink record carries a millisecond UTC ISO 8601 timestamp |
| Loopback HTTP | real, tested | `include/lucent/http.h`, `src/http.cpp` | One request per connection; no TLS, remote binding, keep-alive, or chunked transfer by design |
| Touch routing | real, tested | `include/lucent/touch.h`, `src/touch.cpp` | Platform shells still own event acquisition and application action mapping; Android/SDL3 bindings consume stable zone events |
| ZIP install extraction | real, tested | `include/lucent/zip.h`, `src/zip.cpp` | Optional `lucent::zip` target uses zlib; it extracts safe stored/deflate entries and finds one required executable |
| Verification | real | `tests/`, `tools/check_cpp_quality.sh`, `tools/check_source_structure.py` | Run through CTest in top-level builds |

## Source tree

```text
include/lucent/  public configuration, logging, HTTP, touch, and ZIP interfaces
src/             one implementation translation unit per public subsystem
tests/           production-interface unit and loopback integration tests
tools/           non-mutating quality and structure gates
```

## Where is X?

- Read a typed environment value: `lucent::config` in `include/lucent/config.h`.
- Emit or capture a diagnostic: `lucent::log` in `include/lucent/log.h`.
- Add a local interactive control channel: `lucent::http::Server` in `include/lucent/http.h`; the
  consumer supplies the route handler.
- Route Android/SDL3 touch contacts into stable virtual-control zones: `lucent::touch::Router` in
  `include/lucent/touch.h`; the consumer supplies the layout, safe-area policy, and action mapping.
- Extract a user-selected ZIP install safely: `lucent::zip::extract_install` in
  `include/lucent/zip.h`; the consumer supplies the required executable basename and destination.

# Codemap

Lucent is a dependency-free C++20 infrastructure library. Each public header has one cohesive
implementation owner; consumers compose the pieces and retain all application-specific policy.
Clang is the verification toolchain; compatible GCC and AppleClang consumers are accepted.

| Subsystem | Status | Where | Gap/next |
|---|---|---|---|
| Configuration | real, tested | `include/lucent/config.h`, `src/config.cpp` | Cached environment configuration only; no file format |
| Logging | real, tested | `include/lucent/log.h`, `src/log.cpp`, `tests/test_core_portability.cpp` | Thread-safe text sinks; every stderr, file, and installed-sink record carries a millisecond UTC ISO 8601 timestamp. Raw logging, sinks, channels, and line lifecycle compile without `<format>`; formatted convenience templates are feature-detected. |
| Loopback HTTP | real, tested | `include/lucent/http.h`, `src/http.cpp` | One request per connection; no TLS, remote binding, keep-alive, or chunked transfer by design. Request framing and diagnostics use the raw logging boundary and do not impose the optional formatting API on consumers. |
| Touch routing | real, tested | `include/lucent/touch.h`, `src/touch.cpp` | Platform shells still own event acquisition and application action mapping; Android/SDL3 bindings consume stable zone events |
| Platform user data | real, tested | `include/lucent/platform.h`, `include/lucent/platform_c.h`, `src/platform.cpp` | Android shells must provide an app-private root; URI selection and document copying remain platform-shell responsibilities |
| ZIP install extraction | real, tested | `include/lucent/zip.h`, `src/zip.cpp` | Optional `lucent::zip` target uses zlib and is disabled by default for embedded consumers; it extracts safe stored/deflate entries and finds one required executable |
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
- Resolve a per-application user-data directory: `lucent::platform::user_data_directory` in
  `include/lucent/platform.h`, or the C wrappers in `include/lucent/platform_c.h`.
- Extract a user-selected ZIP install safely: `lucent::zip::extract_install` in
  `include/lucent/zip.h`; the consumer supplies the required executable basename and destination.

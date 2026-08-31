# Codemap

Lucent is a dependency-free C++20 infrastructure library. Each public header has one cohesive
implementation owner; consumers compose the pieces and retain all application-specific policy.
Clang is the verification toolchain; compatible GCC and AppleClang consumers are accepted.

| Subsystem | Status | Where | Gap/next |
|---|---|---|---|
| Configuration | real, tested | `include/lucent/config.h`, `src/config.cpp` | Cached environment configuration only; no file format |
| Content identity | real, tested | `include/lucent/content.h`, `src/content.cpp`, `tests/test_content.cpp` | Dependency-free SHA-256 for memory and streaming files; consumers own expected digests and title policy |
| Logging | real, tested | `include/lucent/log.h`, `src/log.cpp`, `tests/test_core_portability.cpp` | Thread-safe text sinks; every stderr, file, and installed-sink record carries a millisecond UTC ISO 8601 timestamp. Raw logging, sinks, channels, and line lifecycle compile without `<format>`; formatted convenience templates are feature-detected. |
| Loopback HTTP | real, tested | `include/lucent/http.h`, `src/http.cpp` | One request per connection; no TLS, remote binding, keep-alive, or chunked transfer by design. Request framing and diagnostics use the raw logging boundary and do not impose the optional formatting API on consumers. |
| Touch routing | real, tested | `include/lucent/touch.h`, `src/touch.cpp` | Stable zone events retain both capture origin and current position for relative gestures; platform shells still own acquisition and application action mapping |
| Platform user data and Android imports | real, partly platform-compiled | `include/lucent/platform.h`, `include/lucent/platform_c.h`, `src/platform.cpp`, `platforms/android/java/io/github/someoneisworking/lucent/LucentDocumentImport.java` | Android shells provide their app-private root. Lucent owns persisted SAF read grants, bounded private staging, safe rejection discard, source-document disposal after title validation, and recovery-safe promotion of a title-validated staging directory; a title owns selection wording and identity/completeness validation. |
| ZIP install extraction | real, tested | `include/lucent/zip.h`, `src/zip.cpp`, `tests/test_zip.cpp` | Optional `lucent::zip` target uses zlib and is disabled by default for embedded consumers. Path and byte inputs support direct archives or one nested ZIP, content-identity selection across both levels, aggregate archive/entry/expanded-size budgets, path/duplicate/CRC validation, and failure-atomic publication into a fresh destination. |
| Verification | real | `tests/`, `tools/check_cpp_quality.sh`, `tools/check_source_structure.py` | Run through CTest in top-level builds |

## Source tree

```text
include/lucent/  public configuration, content, logging, HTTP, touch, and ZIP interfaces
src/             one implementation translation unit per public subsystem
platforms/android/java/ title-neutral Android Activity and SAF import mechanics
tests/           production-interface unit and loopback integration tests
tools/           non-mutating quality and structure gates
```

## Where is X?

- Read a typed environment value: `lucent::config` in `include/lucent/config.h`.
- Hash file content without loading it all into memory: `lucent::content::sha256_file` in
  `include/lucent/content.h`.
- Emit or capture a diagnostic: `lucent::log` in `include/lucent/log.h`.
- Add a local interactive control channel: `lucent::http::Server` in `include/lucent/http.h`; the
  consumer supplies the route handler.
- Route Android/SDL3 touch contacts into stable virtual-control zones: `lucent::touch::Router` in
  `include/lucent/touch.h`; the consumer supplies the layout, safe-area policy, and action mapping.
- Resolve a per-application user-data directory: `lucent::platform::user_data_directory` in
  `include/lucent/platform.h`, or the C wrappers in `include/lucent/platform_c.h`.
- Extract a user-selected ZIP install safely: `lucent::zip::extract_unique_install` in
  `include/lucent/zip.h` searches direct entries and one nested ZIP through a consumer-owned content
  matcher, then atomically publishes the archive level containing the unique match. Use
  `extract_archive` when discovery is not needed; `extract_install` remains the exact-basename
  convenience path.

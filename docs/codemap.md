# Codemap

Lucent is a dependency-free C++20 infrastructure library. Each public header has one cohesive
implementation owner; consumers compose the pieces and retain all application-specific policy.
Clang is the verification toolchain; compatible GCC and AppleClang consumers are accepted.

| Subsystem | Status | Where | Gap/next |
|---|---|---|---|
| Configuration (environment) | real, tested | `include/lucent/config.h`, `src/config.cpp` | Cached environment configuration only; the primitive `cvar` reads its environment layer from |
| Layered CVars | real; base layering tested, new parts untested | `include/lucent/cvar.hpp`, `include/lucent/cvar_c.h`, `src/cvar.cpp`, `tests/test_cvar.cpp` | `Var<T>` (bool/long/double/string) with layers default < file (`name = value`) < environment or `--set` < runtime, each layer queryable (`has`, `at`, `layer_string`, `clear`); `Persistence::RunOnly` vars never touch the file; `Options::env_name` binds an exact environment name instead of the prefixed one; a value that does not parse is logged and ignored. C ABI for C consumers; unknown file keys preserved on rewrite. The runtime layer, per-layer queries, `RunOnly`, `env_name` and the parse warning have no test. Change subscriptions not ported |
| Content identity | real, tested | `include/lucent/content.h`, `src/content.cpp`, `tests/test_content.cpp` | Dependency-free SHA-256 for memory and streaming files; consumers own expected digests and title policy |
| Linux native file selection | Optional GTK3 owner | `include/lucent/file_dialog.h`, `src/file_dialog.cpp`, `tests/test_file_dialog.cpp` | `lucent::file_dialog::FileDialog` owns one main-thread native chooser, nonblocking polling, and cancellation/teardown. Consumers own selection wording and file interpretation. |
| Linux asynchronous file reading | Optional GIO owner | `include/lucent/file_read.h`, `src/file_read.cpp`, `src/file_read_open.h`, `src/file_read_open.cpp`, `tests/test_file_read.cpp` | `lucent::file_read::FileRead` owns bounded asynchronous reads and cancellation lifetime; the private open seam owns nonblocking descriptor acquisition and regular-file validation. Consumers choose the byte budget and parse content. Both Linux file owners link through optional `lucent::file_dialog`. |
| Immutable native backing files | Optional GIO worker owner | `include/lucent/file_store.h`, `src/file_store.cpp`, `src/file_store_directory.h`, `src/file_store_directory.cpp`, `tests/test_file_store.cpp` | `lucent::file_store::FileStore` owns asynchronous byte staging, aggregate reservations, and result lifetime. Its private directory peer owns cryptographic names, exclusive creation, permissions, writes, and exact-file cleanup. Consumers choose bytes/suffix/budgets and release native readers before terminal store teardown. |
| Logging | real, tested | `include/lucent/log.h`, `src/log.cpp`, `tests/test_core_portability.cpp` | Thread-safe text sinks; every stderr, file, and installed-sink record carries a millisecond UTC ISO 8601 timestamp. Raw logging, sinks, channels, and line lifecycle compile without `<format>`; formatted convenience templates are feature-detected, and a formatter that throws is reported in the logged line instead of escaping the level helper (`tests/test_lucent.cpp`). |
| Bounded HTTP | real, tested | `include/lucent/http.h`, `src/http.cpp` | Loopback is the default. An explicit local-network scope binds all IPv4 interfaces for authenticated, opt-in sharing routes; Lucent supplies no authentication or TLS. Handlers see the request headers (`Request::header`, case-insensitive) and may add response headers, refused with a 500 if they hold a line break. Responses can stream a consumer-authorised regular file in bounded blocks; Lucent does not choose files or authorize paths. One request per connection; no keep-alive or chunked transfer. Request framing and diagnostics use the raw logging boundary and do not impose the optional formatting API on consumers. |
| Loopback HTTP client | real, tested | `include/lucent/http_client.h`, `src/http_client.cpp` | One GET or POST to 127.0.0.1 (Content-Length or close framing; chunked refused) and a text-only client WebSocket with an RFC 6455 handshake check, ping replies, fragment reassembly and a 16 MiB message bound. For local debugging protocols such as Chrome DevTools; no TLS, no remote hosts. |
| Touch routing | real, tested | `include/lucent/touch.h`, `src/touch.cpp` | Stable zone events retain both capture origin and current position for relative gestures. The C++ router owns stable capture semantics; Android lifecycle and contact acquisition belong to the separate shared android-port framework; titles own safe-area layout and game-action mapping. |
| Text primitives | real; case folding tested, `trim` and `json_quote` untested | `include/lucent/text.h`, `src/text.cpp`, `tests/test_text.cpp` | `ascii_lower` and `ascii_iequals` share deterministic ASCII-only case semantics for configuration tokens and ZIP names without a locale-sensitive transform; non-ASCII bytes remain unchanged. `trim` strips C-locale whitespace for config, file and header parsing. `json_quote` returns a JSON string literal with control bytes escaped. |
| ZIP install extraction | real, tested | `include/lucent/zip.h`, `src/zip.cpp`, `src/zip_reader.{h,cpp}`, `src/zip_directory.{h,cpp}`, `src/zip_entry.{h,cpp}`, `tests/test_zip*.cpp` | Optional `lucent::zip` target uses zlib and is disabled by default for embedded consumers. The reader owns bounded random-access file/span I/O; the directory owner validates metadata, names and aggregate budgets; the entry owner streams stored/deflate bytes and CRC; `zip.cpp` owns selection and failure-atomic publication. Filename selection and whole-archive extraction never map or buffer complete files or entries. The content-matcher span API retains its explicit per-entry buffer bound and one nested ZIP; `read_single_entry` reads a one-file archive into memory under the same entry bound. |
| Verification | real | `tests/`, `tools/check_cpp_quality.py`, `tools/check_source_structure.py` | Run through CTest in top-level builds |
| Pinned GTK runtime provisioning | Optional Linux dependency build owner | `dependencies/gtk.json`, `tools/gtk_runtime.py`, `tools/gtk_prerequisites.py`, `tests/test_gtk_runtime.py` | The immutable source contract and Meson/Ninja recipe build X11 and Wayland GTK into a consumer's `build/deps` prefix. Consumers acquire the declared Git revision and invoke the CLI through their locked Python interpreter; Lucent validates clean source identity, prerequisites, configured compiler/options, and installed prefix. It does not run tests or install system packages during provisioning. |
| Hosted native verification | target-neutral CI orchestration | `.github/workflows/ci.yml` | CMake/Ninja and CTest on each supported desktop host |

## Source tree

```text
include/lucent/  public configuration, content, logging, HTTP, touch, and ZIP interfaces
src/             one implementation translation unit per public subsystem
tests/           production-interface unit and loopback integration tests
tools/           non-mutating Python quality and structure gates
```

## Where is X?

- Read a typed environment value: `lucent::config` in `include/lucent/config.h`.
- Trim text or quote it as JSON: `lucent::text::trim` / `json_quote` in `include/lucent/text.h`.
- Define a program setting with a config file / environment / `--set` precedence: `lucent::cvar::Var<T>`
  in `include/lucent/cvar.hpp` (`lucent_cvar_*` in `cvar_c.h` from C).
- Hash file content without loading it all into memory: `lucent::content::sha256_file` in
  `include/lucent/content.h`.
- Select one local Linux file without blocking the host loop: `lucent::file_dialog::FileDialog`
  in `include/lucent/file_dialog.h`; call `poll()` from the main loop.
- Read a selected local file asynchronously within a consumer-defined byte budget:
  `lucent::file_read::FileRead` in `include/lucent/file_read.h`.
- Supply immutable local paths to native readers of in-memory content: `lucent::file_store::FileStore`
  in `include/lucent/file_store.h`; it publishes only completely written files and deletes owned paths
  at terminal teardown, without depending on a subsequent GLib iteration.
- Build the maintained GTK runtime used by native file adapters: the consumer provisions the exact
  source in `dependencies/gtk.json`, then invokes `tools/gtk_runtime.py --source PATH --build PATH
  --prefix PATH` with distinct sibling paths under its own `build/deps`. The caller's interpreter
  must provide locked Meson; the source is never downloaded, switched, or copied by this builder.
  The prefix supplies `lib/pkgconfig`, GTK/GDK shared libraries, headers, and compiled schemas.
  Consumers own runtime library selection plus `GSETTINGS_SCHEMA_DIR`/`XDG_DATA_DIRS` composition.
- Emit or capture a diagnostic: `lucent::log` in `include/lucent/log.h`.
- Talk to a local debugging endpoint or control channel (Chrome DevTools and similar): `lucent::http::get_loopback`, `post_loopback`
  and `lucent::http::WebSocketClient` in `include/lucent/http_client.h`.
- Add a local interactive control channel: `lucent::http::Server` in `include/lucent/http.h`; the
  consumer supplies the route handler. `ListenScope::LocalNetwork` is for explicitly authenticated,
  user-visible sharing only; ordinary control channels use the default loopback scope.
- Stream a consumer-authorised local download: return `lucent::http::Response::file` from the
  `lucent::http::Server` handler. The consumer owns capability checks, path ownership, and offer
  lifetime; Lucent only frames and streams the already-authorised file.
- Route Android/SDL3 touch contacts into stable virtual-control zones: `lucent::touch::Router` in
  `include/lucent/touch.h`; the consumer supplies the layout, safe-area policy, and action mapping.
- Resolve a per-application user-data directory: `lucent::platform::user_data_directory` in
  `include/lucent/platform.h`, or the C wrappers in `include/lucent/platform_c.h`. Per-OS data,
  cache and runtime directories from a supplied `Environment`: `resolve_data_directory`,
  `resolve_cache_directory`, `resolve_runtime_directory` in the same header.
- Extract a user-selected ZIP install safely: `lucent::zip::extract_unique_install` in
  `include/lucent/zip.h` searches direct entries and one nested ZIP through a consumer-owned content
  matcher, then atomically publishes the archive level containing the unique match. Use
  `extract_archive` when discovery is not needed; `extract_install` remains the exact-basename
  convenience path.

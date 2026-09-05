# lucent

A small C++20 library for the infrastructure every program needs and otherwise reinvents badly:
**logging**, **configuration**, and a **loopback HTTP control channel**.

The core has no third-party dependencies. Optional targets add zlib-backed ZIP extraction and
Linux GTK3/GIO file selection and bounded asynchronous reading.

## Optional Linux file access

Enable `LUCENT_BUILD_FILE_DIALOG=ON` before adding Lucent and link `lucent::file_dialog`.
This requires GTK 3.20+ development headers and pkg-config (`libgtk-3-dev` on Debian/Ubuntu,
`gtk3-devel` on Fedora). The core remains independent of GTK. This target currently supports
Linux desktops only; Android SAF is owned by `LucentDocumentImport`, and no Windows/macOS
picker implementation is claimed.

`lucent::file_dialog::FileDialog` opens one native single-file chooser and dispatches its
selected/cancelled/error result from `poll()` after native teardown. Call every method and
destroy the owner on the application main thread. The completion can safely open a new request
or destroy its owner. `cancel()` defers a cancelled result to the next poll; `close()` and the
destructor suppress completion. The selected path is not read or interpreted by the picker.

`lucent::file_read::FileRead` in `<lucent/file_read.h>` separately reads a regular file through
GIO async operations, enforcing a caller-supplied byte limit both against metadata and during
the read. `poll()` returns a completed `{bytes, error}` once; any error discards partial bytes.
An I/O worker opens with `O_NONBLOCK` and validates the opened descriptor before creating the GIO
stream, so a path replaced with a FIFO cannot block open or evade regular-file checking.
`close()` cancels and detaches the request. Late callbacks own only internal shared state, not
the reader or its consumer. Continue main-loop polling to dispatch cancelled GIO completions;
the reader's `poll()` services the context even while idle. Parsing and save publication remain
consumer responsibilities.

For this optional target's tests, install Xvfb and xauth, then configure with
`cmake -S . -B build/file-dialog -G Ninja -DLUCENT_BUILD_FILE_DIALOG=ON`, build, and run
`ctest --test-dir build/file-dialog --output-on-failure`. Tests use an isolated virtual display,
real GTK chooser responses, synthetic files, and the normal format/lint/structure checks.

```cpp
#include <lucent/log.h>
#include <lucent/config.h>

lucent::config::set_prefix("MYAPP_");            // "WIDE" now reads MYAPP_WIDE

if (lucent::config::flag("WIDE")) enable_widescreen();
int frames = lucent::config::number("FRAMES", 60);

lucent::info ("boot", "loaded {} ({} bytes)", path, size);
lucent::warn ("cd",   "{} missing — extracting from disc", path);
lucent::error("boot", "cannot resolve {}", path);
lucent::debug("gpu",  "prim {} at ({}, {})", i, x, y);   // silent unless the channel is on
```

For an interactive local control plane, the consumer owns routes while Lucent owns sockets,
request limits, concurrent dispatch, response framing, and shutdown:

```cpp
#include <lucent/http.h>

lucent::http::Server server({.port = 32123}, [](const lucent::http::Request& request) {
  if (request.method == "GET" && request.path() == "/status") {
    return lucent::http::Response::json(200, "OK", R"({"ready":true})");
  }
  return lucent::http::Response::text(404, "Not Found", "unknown endpoint\n");
});
if (!server.start()) return 1;
```

The listener binds strictly to `127.0.0.1`. Requests are bounded and handled concurrently, so a
slow screenshot or probe route does not prevent another connection from delivering input. This is
a control channel, not a general web framework: one request per connection, no remote binding, TLS,
keep-alive, or chunked transfer encoding.

```
[2026-08-25T15:42:17.083Z] [boot] loaded assets/main.bin (716800 bytes)
[2026-08-25T15:42:17.084Z] [cd:warn] assets/extra.bin missing — extracting from disc
[2026-08-25T15:42:17.084Z] [boot:error] cannot resolve assets/missing.bin
```

## Why

Most codebases grow two messes in parallel.

The **config** mess is `getenv` scattered through the code, each site with its own caching hack and
its own idea of what `"0"` means:

```cpp
static int s_wide = -1;
if (s_wide < 0) { const char* e = getenv("MYAPP_WIDE"); s_wide = e && *e != '0'; }
```

The **logging** mess is worse, because it starts reasonable. A `fprintf(stderr, ...)` here, a
`if (verbose) printf(...)` there. It looks fine until you want to send output to a file, or capture it
in a test, or turn one noisy subsystem off — and discover that half your output bypasses whatever
mechanism you added, because it was never routed through anything.

lucent's actual value is not the formatting. It is that **there is one path out**, so redirecting,
capturing, or silencing output works on all of it.

## The one rule

Choose by **audience**, not by taste:

| Audience | Use | Emitted |
|---|---|---|
| Something a normal run should print | `info` / `warn` / `error` | always |
| Something shown only when asked for | `debug("channel", …)` | only when the channel is on |

On a hot path use `debug(channel_handle, …)` — see [Hot call sites](#hot-call-sites-channel).

If you find yourself writing `if (verbose) fprintf(stderr, …)`, that is a `debug` channel.

Enable channels from the environment — `LUCENT_DEBUG=cd,gpu`, or `LUCENT_DEBUG=all` — or at runtime
via `lucent::enable_channels("cd,gpu")`, which is what a debug console should call.

### Naming the variables yourself

If your users already type something other than `LUCENT_DEBUG`, rename it **at build time**:

```cmake
set(LUCENT_CHANNEL_ENV  "MYAPP_DEBUG")     # replaces <prefix>LUCENT_DEBUG
set(LUCENT_LOG_FILE_ENV "MYAPP_LOG_FILE")  # replaces <prefix>LUCENT_LOG_FILE
add_subdirectory(vendor/lucent)            # or FetchContent_MakeAvailable(lucent)
```

Build time and not an init call, deliberately. Both variables are resolved **lazily, on the first log
call**, so a baked-in name is already correct for the very first `lucent::debug()` in the process —
including one from a static initialiser. A setter would instead have to be *invoked by something*,
and whatever logged before it ran would be dropped with no message: logging that works only because
some unrelated code happens to run first is logging that will one day stop working for no visible
reason. `lucent::config::set_channel_env()` exists for tests and late reconfiguration — it re-reads
the environment rather than being ignored — but it cannot recover a line already dropped.

An explicit `enable_channels()` always outranks the environment, whichever variable that is.

## Hot call sites: `Channel`

With **no** channel enabled, `debug("gpu", …)` is one relaxed atomic load and a branch — genuinely
free. With **any** channel enabled it has to hash the name, so it takes the mutex and builds a
`std::string`. That is fine for ordinary code and wrong for a call site that runs per instruction,
per packet or per pixel — where it bites precisely when someone is debugging.

Hoist the name into a `Channel` and the gate becomes a load, a compare and a branch:

```cpp
static const lucent::Channel ch{"otattr"};   // constant-initialised; no guard variable
if (ch) { ...work you only do when diagnosing... }
lucent::debug(ch, "store {:08X}", addr);     // info/warn/error take a Channel too
```

Measured (gcc 15 `-O3`, x86-64, 2M iterations, one channel **on** and the measured channel **off**,
median of five runs):

| | string_view | `Channel` | |
|---|---|---|---|
| gate (`channel_on` vs `if (ch)`) | 19.4 ns | 0.67 ns | ~29x |
| whole `debug(…)` site | 18.3 ns | 0.66 ns | ~28x |

The mutex is uncontended in that benchmark, so ~29x is the floor: with several threads logging, the
string_view path degrades and the `Channel` path does not — it never takes the lock.

A `Channel` is **not** a stale snapshot. Every `enable_channels()` / `enable_channel()` bumps a
global generation that invalidates every `Channel` in the program at once, so a debug console's
`debug gpu` typed mid-run takes effect on the next call. A `Channel` constructed — or read — during
static initialisation, before the environment has been consulted, is handled too. The name is
borrowed, not copied: pass a string literal or something that outlives the `Channel`.

## Configuration

| Call | Meaning |
|---|---|
| `config::flag("WIDE")` | present and not `0`/`false`/`no`/`off` |
| `config::number("PORT", 8080)` | integer; accepts `0x` hex; falls back if absent or unparseable |
| `config::text("LOG_FILE")` | string; empty when absent |
| `config::present("WIDE")` | set at all, whatever the value |
| `config::active()` | every prefixed variable, sorted — for a startup banner |

Each name is read from the environment once and cached. `flag` deliberately collapses
present-but-empty to `false`; use `present` when you need to tell "unset" from "set to nothing".

## Platform user data

`lucent::platform::user_data_directory("myapp")` resolves the conventional private configuration
directory: `XDG_CONFIG_HOME` or `$HOME/.config` on Linux, `$HOME/Library/Application Support` on
macOS, and `APPDATA` on Windows. The application name is validated as one path component. Call
`ensure_user_data_directory` after resolution to create it with owner-only permissions where the
platform supports them.

Android shells have no environment fallback. An Activity supplies its absolute app-private root
through `lucent_platform_set_user_data_directory()` from `platform_c.h`; the C wrappers are for
applications whose entry point is written in C. Lucent does not own document pickers, URI copying,
or application-specific install validation.

## Where output goes

By default stderr, or a file if `LUCENT_LOG_FILE` is set (appended, line-buffered so `tail -f` works
and a crash does not swallow the last lines). Every emitted record begins with a millisecond UTC
timestamp in ISO 8601 form, so output from different hosts can be correlated without guessing their
local time zones.

In tests, install a sink and assert on real output:

```cpp
std::vector<std::string> lines;
lucent::set_sink([&](lucent::Level, std::string_view line) { lines.emplace_back(line); });
lucent::info("cd", "loaded {} bytes", 12);
assert(lines[0].ends_with("Z] [cd] loaded 12 bytes"));
lucent::set_sink(nullptr);   // restore the default
```

## Lines built in pieces

A hex dump, a register grid, a row of counters — assembled in a loop, but still one line. The logger
emits one line per call, so accumulate and flush:

```cpp
lucent::Line row;
row.add("  {:08x}:", addr);
for (auto b : bytes) row.add(" {:02x}", b);
row.flush(lucent::Level::Info, "mem");
```

Truncation-safe: past 4096 characters it appends `...` and stops accepting more. An empty `Line`
flushes to nothing, so a loop that produced no pieces stays silent rather than printing a bare tag.

A leading `\n` in a message is treated as a blank-line separator and emitted *before* the tag, so
banners do not strand the tag on the previous line.

## Using it

With FetchContent:

```cmake
include(FetchContent)
FetchContent_Declare(lucent
  GIT_REPOSITORY https://github.com/SomeoneIsWorking/lucent.git
  GIT_TAG main)
FetchContent_MakeAvailable(lucent)
target_link_libraries(myapp PRIVATE lucent::lucent)
```

Or as a subdirectory / submodule:

```cmake
add_subdirectory(external/lucent)
target_link_libraries(myapp PRIVATE lucent::lucent)
```

The zlib-backed `lucent::zip` target is built by default only when Lucent is the top-level project.
An embedded consumer that needs it enables `LUCENT_BUILD_ZIP` before `add_subdirectory`; core-only
consumers do not need zlib. `lucent::zip::extract_install` accepts configurable compressed-size,
expanded-size, per-entry, and entry-count limits and validates all entries before writing output.

Requires a conforming C++20 compiler. Raw `log()`, channels, sinks, HTTP, platform, and touch APIs
do not require `std::format`; the formatted `info`/`warn`/`error`/`debug` and `Line::add` convenience
templates are exposed when the consumer's standard library provides `<format>` and
`__cpp_lib_format`. This keeps core consumers compatible with libstdc++ 11 without substituting a
second formatting implementation. Development verification uses Clang, but Lucent does not reject
compatible GCC or AppleClang consumer toolchains. Tests only build when lucent is the top-level
project:

```sh
cmake -S . -B build && cmake --build build && ./build/lucent_tests
```

## Threading

Emitting is mutex-guarded, so lines from different threads do not interleave. `set_prefix` should
still be called once at startup, before other threads are reading config — it now discards the
cached lookups and re-arms the channel set, so a late call takes effect rather than half-applying,
but a `debug()` that already ran under the wrong prefix is gone.

The channel gates — both the no-channels-enabled fast path and a `Channel` handle — read relaxed
atomics without the lock. A reader racing a concurrent `enable_channels()` gets the previous answer
for one call and re-resolves on the next; nothing tears. The test suite exercises this with four
reader threads against a toggling writer, and passes clean under `-fsanitize=thread`.

lucent's own state is constructed on first use and never destroyed, so it is safe to log from a
static initialiser or a static destructor — the first line of the program to the last.

## License

MIT.

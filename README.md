# lucent

A small C++20 library for the two things every program needs and every program reinvents badly:
**logging** and **configuration**.

No dependencies. Two headers. Roughly 300 lines of implementation.

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

```
[boot] loaded assets/main.bin (716800 bytes)
[cd:warn] assets/extra.bin missing — extracting from disc
[boot:error] cannot resolve assets/missing.bin
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

If you find yourself writing `if (verbose) fprintf(stderr, …)`, that is a `debug` channel.

Enable channels from the environment — `LUCENT_DEBUG=cd,gpu`, or `LUCENT_DEBUG=all` — or at runtime
via `lucent::enable_channels("cd,gpu")`, which is what a debug console should call.

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

## Where output goes

By default stderr, or a file if `LUCENT_LOG_FILE` is set (appended, line-buffered so `tail -f` works
and a crash does not swallow the last lines).

In tests, install a sink and assert on real output:

```cpp
std::vector<std::string> lines;
lucent::set_sink([&](lucent::Level, std::string_view line) { lines.emplace_back(line); });
lucent::info("cd", "loaded {} bytes", 12);
assert(lines[0] == "[cd] loaded 12 bytes");
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

Requires C++20 (for `std::format`). Tests only build when lucent is the top-level project:

```sh
cmake -S . -B build && cmake --build build && ./build/lucent_tests
```

## Threading

Emitting is mutex-guarded, so lines from different threads do not interleave. `set_prefix` should be
called once at startup, before any lookup — values are cached, so changing it afterwards will not
re-read what has already been read.

## License

MIT.

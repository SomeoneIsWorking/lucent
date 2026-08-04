// lucent/log.h — one logger, four levels, named channels.
//
//     lucent::info ("boot", "loaded {} ({} bytes)", path, size);
//     lucent::warn ("cd",   "{} missing — extracting from disc", path);
//     lucent::error("boot", "cannot resolve {}", path);
//     lucent::debug("gpu",  "prim {} at ({}, {})", i, x, y);   // only when the channel is on
//
// Output is `[channel] message`, one line per call — the newline is added for you, and message text
// should not contain the channel name or a trailing newline.
//
// THE RULE THIS HEADER EXISTS TO ENFORCE: pick by AUDIENCE, not by taste.
//   * Something a normal run should print  -> info / warn / error. Always emitted.
//   * Something shown only when asked for  -> debug("channel", ...). Off unless enabled.
// Anything else — a bare printf, a std::cout, a `if (verbose) fprintf(...)` two-step — bypasses the
// sink, so it escapes log-to-file, escapes capture in tests, and cannot be turned off.
//
// Channels are enabled from the environment: LUCENT_DEBUG=cd,gpu  (or LUCENT_DEBUG=all). The
// variable respects lucent::config's prefix, so an app with prefix "MYAPP_" reads MYAPP_LUCENT_DEBUG.
// Output goes to stderr unless LUCENT_LOG_FILE names a file, or a sink is installed in code.
//
// COST — measured, not asserted (see test_channel_gate_is_measurably_cheaper in tests/):
//   * With NO channel enabled at all, debug("chan", ...) is one relaxed atomic load and a branch.
//     That is the usual case, and it is genuinely free.
//   * With ANY channel enabled — i.e. exactly when someone is debugging — the string_view form has
//     to hash the name, so it takes the mutex and builds a std::string. It is NOT one bool test.
//   * On a hot path, hoist the name into a `static const lucent::Channel` (below).
//
//   Measured, gcc 15 -O3, x86-64, single-threaded, 2M iterations, one channel ON and the measured
//   channel OFF (the case that hurts), median of five runs:
//
//       channel_on("otattr")            19.4 ns/call        debug("otattr", ...)   18.3 ns/call
//       Channel gate  `if (ch)`          0.67 ns/call       debug(ch, ...)          0.66 ns/call
//                                       ~29x cheaper                               ~28x cheaper
//
//   That mutex is uncontended in this benchmark, so 29x is the FLOOR: with several threads logging,
//   the string_view path degrades and the Channel path does not (it never takes the lock).
#pragma once

#include <atomic>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>

namespace lucent {

enum class Level { Debug, Info, Warn, Error };

// ── Channel: a name resolved once, for hot call sites ───────────────────────────────────────────
// The string_view helpers below hash the channel name on every call. That is fine for the ordinary
// call site and wrong for one that runs per guest instruction, per packet, per pixel. A Channel
// resolves the name once and caches the answer next to a stamp of the enabled-channel set, so the
// steady-state gate is a load, a compare and a branch:
//
//     static const lucent::Channel ch{"otattr"};   // constant-initialised; no guard variable
//     if (ch) { ...work you only do when diagnosing... }
//     lucent::debug(ch, "store {:08X}", addr);
//
// It is NOT a stale snapshot: every enable_channels() / enable_channel() bumps a global generation,
// which invalidates every Channel in the program at once, so a REPL `debug gpu` typed mid-run takes
// effect on the next call. A Channel built (and even read) before the environment has been loaded
// is handled — it simply misses once more.
//
// The name is borrowed, not copied: pass a string literal or something that outlives the Channel.
namespace detail {
// Bumped under the log mutex whenever the enabled set changes; read without the mutex. Declared here
// only so the fast path inlines — treat it as private.
extern std::atomic<std::uint64_t> g_channel_generation;
}  // namespace detail

class Channel {
 public:
  constexpr explicit Channel(std::string_view name) : name_(name) {}
  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;

  constexpr std::string_view name() const { return name_; }

  // True while this channel is emitting. One relaxed load of the global generation, one relaxed load
  // of the cached word, one compare.
  explicit operator bool() const { return enabled(); }
  bool enabled() const {
    const std::uint64_t gen = detail::g_channel_generation.load(std::memory_order_relaxed);
    const std::uint64_t cached = state_.load(std::memory_order_relaxed);
    if ((cached >> 1) == gen) return (cached & 1u) != 0;
    return resolve();
  }

 private:
  // Cold: takes the mutex, resolves the name, re-stamps. Out of line so the hot path stays tiny.
  bool resolve() const;

  std::string_view name_;
  // Generation stamp and answer packed into ONE word, deliberately. Two separate atomics could be
  // read half-updated — a reader seeing the new stamp beside the old answer would cache a wrong
  // value and, since the stamp now matches, never look again. That is a permanent wrong answer, not
  // the one-call staleness the rest of this file tolerates. A single relaxed word cannot tear.
  // Starts at 0 (generation 0, answer false) while the global generation starts at 1, so the very
  // first read always misses and goes to resolve(), whatever ran before it.
  mutable std::atomic<std::uint64_t> state_{0};
};

// ── Emitting ────────────────────────────────────────────────────────────────────────────────────
// The formatting entry point. Prefer the level helpers below; this exists for code that computes a
// level at runtime.
void log(Level level, std::string_view channel, std::string_view message);

namespace detail {
// The gate behind debug(). Free when no channel is enabled anywhere; a hashed lookup under a mutex
// once any channel is on — see the COST note at the top, and use a Channel on a hot path. Kept out
// of the public surface so the helpers below read cleanly.
bool channel_enabled(std::string_view channel);
}  // namespace detail

template <class... Args>
void info(std::string_view channel, std::format_string<Args...> fmt, Args&&... args) {
  log(Level::Info, channel, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void warn(std::string_view channel, std::format_string<Args...> fmt, Args&&... args) {
  log(Level::Warn, channel, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void error(std::string_view channel, std::format_string<Args...> fmt, Args&&... args) {
  log(Level::Error, channel, std::format(fmt, std::forward<Args>(args)...));
}
// Channel-gated. The format arguments are NOT evaluated when the channel is off.
template <class... Args>
void debug(std::string_view channel, std::format_string<Args...> fmt, Args&&... args) {
  if (!detail::channel_enabled(channel)) return;
  log(Level::Debug, channel, std::format(fmt, std::forward<Args>(args)...));
}

// The same four, taking a resolved Channel. Identical output; the debug() one gates on the cached
// word instead of hashing the name. Migrating a call site is a matter of hoisting the literal.
template <class... Args>
void info(const Channel& channel, std::format_string<Args...> fmt, Args&&... args) {
  log(Level::Info, channel.name(), std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void warn(const Channel& channel, std::format_string<Args...> fmt, Args&&... args) {
  log(Level::Warn, channel.name(), std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void error(const Channel& channel, std::format_string<Args...> fmt, Args&&... args) {
  log(Level::Error, channel.name(), std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void debug(const Channel& channel, std::format_string<Args...> fmt, Args&&... args) {
  if (!channel.enabled()) return;
  log(Level::Debug, channel.name(), std::format(fmt, std::forward<Args>(args)...));
}

// ── Channels ────────────────────────────────────────────────────────────────────────────────────
// Usually driven by LUCENT_DEBUG. Call these to change it at runtime — e.g. from a debug console.
bool channel_on(std::string_view channel);           // is this channel currently emitting?
                                                     // (a Channel answers the same, far cheaper)
void enable_channels(std::string_view list);         // "cd,gpu" or "all"; replaces the current set
void enable_channel(std::string_view channel, bool on = true);

// ── Where output goes ───────────────────────────────────────────────────────────────────────────
// By default: LUCENT_LOG_FILE if set (appended, line-buffered), otherwise stderr. Installing a sink
// replaces that — the sink receives the FULLY FORMATTED line including the "[channel] " prefix, minus
// the trailing newline. Passing nullptr restores the default.
using Sink = std::function<void(Level, std::string_view line)>;
void set_sink(Sink sink);

// ── Building a line in pieces ───────────────────────────────────────────────────────────────────
// Some output is a row assembled in a loop — a hex dump, a register grid, a column of counters. The
// logger emits one whole line per call, so those cannot call it per piece. Accumulate, then flush:
//
//     lucent::Line row;
//     row.add("  {:08x}:", addr);
//     for (auto b : bytes) row.add(" {:02x}", b);
//     row.flush(lucent::Level::Info, "mem");
//
// Truncation-safe: past the cap it appends "..." and stops accepting more.
class Line {
 public:
  template <class... Args>
  Line& add(std::format_string<Args...> fmt, Args&&... args) {
    append(std::format(fmt, std::forward<Args>(args)...));
    return *this;
  }
  // Emits the accumulated text as one line and clears it. A no-op when nothing was added, so a loop
  // that produced no pieces prints nothing rather than an empty "[chan] ".
  void flush(Level level, std::string_view channel);
  // Same, but gated on the channel like debug().
  void flush_debug(std::string_view channel);
  void flush_debug(const Channel& channel);

  bool empty() const { return text_.empty(); }
  std::string_view view() const { return text_; }
  void clear() { text_.clear(); truncated_ = false; }

  static constexpr std::size_t kMaxLength = 4096;

 private:
  void append(std::string_view piece);
  std::string text_;
  bool truncated_ = false;
};

}  // namespace lucent

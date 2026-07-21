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
#pragma once

#include <format>
#include <functional>
#include <string>
#include <string_view>

namespace lucent {

enum class Level { Debug, Info, Warn, Error };

// ── Emitting ────────────────────────────────────────────────────────────────────────────────────
// The formatting entry point. Prefer the level helpers below; this exists for code that computes a
// level at runtime.
void log(Level level, std::string_view channel, std::string_view message);

namespace detail {
// Cheap enough to call unconditionally: a disabled channel costs one cached bool test, and the
// arguments are not formatted. Kept out of the public surface so the helpers below read cleanly.
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

// ── Channels ────────────────────────────────────────────────────────────────────────────────────
// Usually driven by LUCENT_DEBUG. Call these to change it at runtime — e.g. from a debug console.
bool channel_on(std::string_view channel);           // is this channel currently emitting?
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

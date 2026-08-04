// lucent/config.h — typed, cached configuration.
//
// Configuration comes from the environment. The point of this header is that a program stops doing
// this, everywhere, forever:
//
//     static int s_wide = -1;
//     if (s_wide < 0) { const char* e = getenv("MYAPP_WIDE"); s_wide = e && *e != '0'; }
//
// and instead says what it means:
//
//     if (lucent::config::flag("WIDE")) ...
//     int frames = lucent::config::number("FRAMES", 60);
//     std::string path = lucent::config::text("LOG_FILE");
//
// Every lookup is read from the environment ONCE and cached, so these are cheap enough to call from
// ordinary code (though not from the innermost loop of a renderer — hoist it into a local there).
//
// Names are given WITHOUT the application prefix. Set the prefix once at startup:
//     lucent::config::set_prefix("MYAPP_");     // then "WIDE" reads MYAPP_WIDE
// The default prefix is empty, so "WIDE" reads WIDE.
//
// ── THE TWO VARIABLES LUCENT READS FOR ITSELF ───────────────────────────────────────────────────
// The logger resolves its own configuration — which debug channels are on, and where output goes —
// from the environment, LAZILY, on the first log call. By default those are `<prefix>LUCENT_DEBUG`
// and `<prefix>LUCENT_LOG_FILE`.
//
// A consumer whose users already type some other name (a port whose every script, doc and .env says
// `PSXPORT_DEBUG`) renames them. THE ONLY ORDER-FREE WAY TO DO THAT IS AT BUILD TIME, so that is the
// primary mechanism — define these when compiling lucent, or set the identically-named CMake
// variables before `add_subdirectory`/`FetchContent_MakeAvailable`:
//
//     LUCENT_CHANNEL_ENV="PSXPORT_DEBUG"        replaces <prefix>LUCENT_DEBUG
//     LUCENT_LOG_FILE_ENV="PSXPORT_LOG_FILE"    replaces <prefix>LUCENT_LOG_FILE
//
// A build-time name needs no initialisation call, so `lucent::debug(...)` on the program's very
// first line — before main, from a static initialiser — already sees the right variable. A runtime
// setter alone cannot promise that: whatever logs before the setter runs is silently lost, and
// "logging works only because something else happens to run first" is precisely the failure this
// library is supposed to remove. The setters below exist for tests and for genuinely late
// reconfiguration; they re-read the environment rather than being ignored, but they cannot recover a
// line that was already dropped.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace lucent::config {

// Prefix applied to every name before it reaches the environment. Changing it DISCARDS every cached
// lookup and re-arms the logger's channel set, so a call after something has already read config (or
// already logged) still takes effect rather than being silently half-applied. It cannot un-drop a
// debug line that was emitted while the wrong prefix was in force — set it early, or better, build
// with LUCENT_CHANNEL_ENV / LUCENT_LOG_FILE_ENV and never call it at all.
void set_prefix(std::string_view prefix);
std::string_view prefix();

// The environment variable naming the enabled debug channels ("cd,gpu" or "all"). Empty name — the
// default — means `<prefix>LUCENT_DEBUG`; a name given here is used VERBATIM, without the prefix,
// precisely so it does not inherit set_prefix's ordering. The compile-time LUCENT_CHANNEL_ENV
// supplies the initial value. Setting it re-reads the environment even if channels were already
// loaded, unless enable_channels() has been called explicitly — an explicit call always wins.
void set_channel_env(std::string_view env_name);
std::string channel_env();          // the full variable name currently in effect
const std::string& channel_list();  // its value ("" when unset), cached like any other lookup

// The same, for the file output goes to. Empty name means `<prefix>LUCENT_LOG_FILE`; initial value
// comes from the compile-time LUCENT_LOG_FILE_ENV. The stream is resolved on first output and not
// re-opened afterwards, so this must be set before the first line if it is to have any effect.
void set_log_file_env(std::string_view env_name);
std::string log_file_env();
const std::string& log_file_path();

// A boolean switch. True when the variable is present and is not "0", "false", "no" or "off"
// (case-insensitive). Absent means false — the safe default for a feature toggle.
bool flag(std::string_view name);

// An integer-valued setting: a frame count, a scale, a port. Returns `fallback` when the variable is
// absent or does not parse. Accepts decimal and 0x-prefixed hex.
long number(std::string_view name, long fallback = 0);

// A string-valued setting: a path, a comma list, a coordinate pair. Empty when absent — check with
// `.empty()` rather than comparing against nullptr.
const std::string& text(std::string_view name);

// True when the variable is present at all, whatever its value. Use this to distinguish "unset" from
// "set to something falsy", which `flag` deliberately collapses.
bool present(std::string_view name);

// Every prefixed variable currently set, as "NAME=value", sorted. For a one-line startup banner
// showing how this run was configured.
std::vector<std::string> active();

// Forget every cached lookup. Exists for tests, which need to set an environment variable and see it
// take effect; production code should not need this.
void reset_cache();

}  // namespace lucent::config

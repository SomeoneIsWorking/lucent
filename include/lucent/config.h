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
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace lucent::config {

// Prefix applied to every name before it reaches the environment. Set once at startup, before any
// lookup — values are cached, so changing it later will not re-read what has already been read.
void set_prefix(std::string_view prefix);
std::string_view prefix();

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

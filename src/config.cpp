#include "lucent/config.h"

#include "lucent/log.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

extern "C" char **environ;

// The names lucent reads for its own two settings. Build-time, because that is the only way for
// them to be right before anything has had a chance to call a setter — see the long note in
// config.h. Empty means "use the default, `<prefix>LUCENT_DEBUG` / `<prefix>LUCENT_LOG_FILE`".
#ifndef LUCENT_CHANNEL_ENV
#define LUCENT_CHANNEL_ENV ""
#endif
#ifndef LUCENT_LOG_FILE_ENV
#define LUCENT_LOG_FILE_ENV ""
#endif

namespace lucent::config {
namespace {

// One lock over the caches. Lookups are cheap and rare relative to real work, and a shared mutex
// buys nothing measurable here; correctness under concurrent first-touch is worth more than the
// contention we are not going to have.
struct State {
  std::mutex mutex;
  std::string prefix;
  std::string channel_env = LUCENT_CHANNEL_ENV;        // "" -> <prefix>LUCENT_DEBUG
  std::string log_file_env = LUCENT_LOG_FILE_ENV;      // "" -> <prefix>LUCENT_LOG_FILE
  std::unordered_map<std::string, std::string> values; // full name -> value ("" also means unset)
  std::unordered_map<std::string, bool> present;
};

// CONSTRUCTED ON FIRST USE AND NEVER DESTROYED, which is not a style choice.
//
// As plain namespace-scope objects these were subject to static-initialisation order across
// translation units: a consumer that logged, or read config, from its own static initialiser could
// reach an unordered_map whose constructor had not run — bucket count zero, hash modulo zero,
// SIGFPE inside operator[]. That is exactly how it failed the first time a Channel was read during
// static init. Deliberately leaking the State also removes the mirror-image hazard at the other end
// of the program: a destructor that logs during static destruction would otherwise touch freed
// containers. A logger has to be usable from the first line of a program to the last; a few hundred
// bytes never returned to the allocator is the price.
State &state() {
  static State *s = new State();
  return *s;
}

std::string full_name(std::string_view name) {
  std::string out(state().prefix);
  out.append(name);
  return out;
}

// Reads the environment once per FULL name (prefix already applied) and remembers the answer,
// including "not set".
const std::string &cached_full(std::string key) {
  State &st = state();
  auto it = st.values.find(key);
  if (it != st.values.end())
    return it->second;
  const char *raw = std::getenv(key.c_str());
  st.present[key] = raw != nullptr;
  return st.values.emplace(std::move(key), raw ? raw : "").first->second;
}

const std::string &cached(std::string_view name) {
  return cached_full(full_name(name));
}

// The variable lucent reads for its own channel list / log file. A name configured by the consumer
// is used verbatim: it must NOT pick up the prefix, or it would inherit set_prefix's ordering,
// which is the very dependency naming it was supposed to remove.
std::string channel_env_locked() {
  State &st = state();
  return st.channel_env.empty() ? st.prefix + "LUCENT_DEBUG" : st.channel_env;
}
std::string log_file_env_locked() {
  State &st = state();
  return st.log_file_env.empty() ? st.prefix + "LUCENT_LOG_FILE" : st.log_file_env;
}

void clear_caches_locked() {
  State &st = state();
  st.values.clear();
  st.present.clear();
}

bool falsey(std::string_view v) {
  std::string lower(v);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return lower == "0" || lower == "false" || lower == "no" || lower == "off";
}

} // namespace

// set_prefix / set_channel_env / set_log_file_env all take the config mutex, DROP IT, and only then
// tell the logger to re-arm. The logger's own mutex is held while it calls back into config (it
// reads the channel list under its lock), so taking the two in the other order here would be a
// textbook lock-order inversion. Nothing between the two statements can observe an inconsistent
// state: the logger re-reads the name when it re-loads.
void set_prefix(std::string_view prefix) {
  {
    State &st = state();
    std::lock_guard lock(st.mutex);
    if (st.prefix == prefix)
      return;
    st.prefix.assign(prefix);
    // Every cached answer was read under the OLD prefix and is now about the wrong variable.
    clear_caches_locked();
  }
  detail::rearm_channels_from_env();
}

void set_channel_env(std::string_view env_name) {
  {
    State &st = state();
    std::lock_guard lock(st.mutex);
    if (st.channel_env == env_name)
      return;
    st.channel_env.assign(env_name);
  }
  detail::rearm_channels_from_env();
}

std::string channel_env() {
  std::lock_guard lock(state().mutex);
  return channel_env_locked();
}

const std::string &channel_list() {
  std::lock_guard lock(state().mutex);
  return cached_full(channel_env_locked());
}

void set_log_file_env(std::string_view env_name) {
  State &st = state();
  std::lock_guard lock(st.mutex);
  st.log_file_env.assign(env_name);
}

std::string log_file_env() {
  std::lock_guard lock(state().mutex);
  return log_file_env_locked();
}

const std::string &log_file_path() {
  std::lock_guard lock(state().mutex);
  return cached_full(log_file_env_locked());
}

std::string_view prefix() {
  State &st = state();
  std::lock_guard lock(st.mutex);
  return st.prefix;
}

bool flag(std::string_view name) {
  std::lock_guard lock(state().mutex);
  const std::string &v = cached(name);
  if (!state().present[full_name(name)])
    return false;
  return !falsey(v);
}

long number(std::string_view name, long fallback) {
  std::lock_guard lock(state().mutex);
  const std::string &v = cached(name);
  if (v.empty())
    return fallback;
  char *end = nullptr;
  const long parsed = std::strtol(v.c_str(), &end, 0); // base 0: decimal, 0x hex, 0 octal
  return (end && end != v.c_str()) ? parsed : fallback;
}

const std::string &text(std::string_view name) {
  std::lock_guard lock(state().mutex);
  return cached(name);
}

bool present(std::string_view name) {
  std::lock_guard lock(state().mutex);
  cached(name);
  return state().present[full_name(name)];
}

std::vector<std::string> active() {
  State &st = state();
  std::lock_guard lock(st.mutex);
  std::vector<std::string> out;
  if (!environ)
    return out;
  for (char **e = environ; *e; ++e) {
    std::string_view entry(*e);
    if (st.prefix.empty() || entry.substr(0, st.prefix.size()) == st.prefix)
      out.emplace_back(entry);
  }
  std::sort(out.begin(), out.end());
  return out;
}

void reset_cache() {
  {
    State &st = state();
    std::lock_guard lock(st.mutex);
    clear_caches_locked();
  }
  // The channel list is one of these cached lookups. Forgetting the cache without re-arming the
  // logger would leave a test that sets LUCENT_DEBUG and calls reset_cache() looking like it had
  // configured nothing — a silently ineffective helper, which is worse than no helper.
  detail::rearm_channels_from_env();
}

} // namespace lucent::config

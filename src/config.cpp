#include "lucent/config.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

extern "C" char** environ;

namespace lucent::config {
namespace {

// One lock over the caches. Lookups are cheap and rare relative to real work, and a shared mutex
// buys nothing measurable here; correctness under concurrent first-touch is worth more than the
// contention we are not going to have.
struct State {
  std::mutex mutex;
  std::string prefix;
  std::unordered_map<std::string, std::string> values;  // full name -> value ("" also means unset)
  std::unordered_map<std::string, bool> present;
};

// CONSTRUCTED ON FIRST USE AND NEVER DESTROYED, which is not a style choice.
//
// As plain namespace-scope objects these were subject to static-initialisation order across
// translation units: a consumer that logged, or read config, from its own static initialiser could
// reach an unordered_map whose constructor had not run — bucket count zero, hash modulo zero, SIGFPE
// inside operator[]. That is exactly how it failed the first time a Channel was read during static
// init. Deliberately leaking the State also removes the mirror-image hazard at the other end of the
// program: a destructor that logs during static destruction would otherwise touch freed containers.
// A logger has to be usable from the first line of a program to the last; a few hundred bytes never
// returned to the allocator is the price.
State& state() {
  static State* s = new State();
  return *s;
}

std::string full_name(std::string_view name) {
  std::string out(state().prefix);
  out.append(name);
  return out;
}

// Reads the environment once per name and remembers the answer, including "not set".
const std::string& cached(std::string_view name) {
  State& st = state();
  std::string key = full_name(name);
  auto it = st.values.find(key);
  if (it != st.values.end()) return it->second;
  const char* raw = std::getenv(key.c_str());
  st.present[key] = raw != nullptr;
  return st.values.emplace(std::move(key), raw ? raw : "").first->second;
}

bool falsey(std::string_view v) {
  std::string lower(v);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return lower == "0" || lower == "false" || lower == "no" || lower == "off";
}

}  // namespace

void set_prefix(std::string_view prefix) {
  State& st = state();
  std::lock_guard lock(st.mutex);
  st.prefix.assign(prefix);
}

std::string_view prefix() {
  State& st = state();
  std::lock_guard lock(st.mutex);
  return st.prefix;
}

bool flag(std::string_view name) {
  std::lock_guard lock(state().mutex);
  const std::string& v = cached(name);
  if (!state().present[full_name(name)]) return false;
  return !falsey(v);
}

long number(std::string_view name, long fallback) {
  std::lock_guard lock(state().mutex);
  const std::string& v = cached(name);
  if (v.empty()) return fallback;
  char* end = nullptr;
  const long parsed = std::strtol(v.c_str(), &end, 0);  // base 0: decimal, 0x hex, 0 octal
  return (end && end != v.c_str()) ? parsed : fallback;
}

const std::string& text(std::string_view name) {
  std::lock_guard lock(state().mutex);
  return cached(name);
}

bool present(std::string_view name) {
  std::lock_guard lock(state().mutex);
  cached(name);
  return state().present[full_name(name)];
}

std::vector<std::string> active() {
  State& st = state();
  std::lock_guard lock(st.mutex);
  std::vector<std::string> out;
  if (!environ) return out;
  for (char** e = environ; *e; ++e) {
    std::string_view entry(*e);
    if (st.prefix.empty() || entry.substr(0, st.prefix.size()) == st.prefix) out.emplace_back(entry);
  }
  std::sort(out.begin(), out.end());
  return out;
}

void reset_cache() {
  State& st = state();
  std::lock_guard lock(st.mutex);
  st.values.clear();
  st.present.clear();
}

}  // namespace lucent::config

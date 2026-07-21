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
std::mutex g_mutex;
std::string g_prefix;
std::unordered_map<std::string, std::string> g_values;  // full name -> value ("" also means unset)
std::unordered_map<std::string, bool> g_present;

std::string full_name(std::string_view name) {
  std::string out(g_prefix);
  out.append(name);
  return out;
}

// Reads the environment once per name and remembers the answer, including "not set".
const std::string& cached(std::string_view name) {
  std::string key = full_name(name);
  auto it = g_values.find(key);
  if (it != g_values.end()) return it->second;
  const char* raw = std::getenv(key.c_str());
  g_present[key] = raw != nullptr;
  return g_values.emplace(std::move(key), raw ? raw : "").first->second;
}

bool falsey(std::string_view v) {
  std::string lower(v);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return lower == "0" || lower == "false" || lower == "no" || lower == "off";
}

}  // namespace

void set_prefix(std::string_view prefix) {
  std::lock_guard lock(g_mutex);
  g_prefix.assign(prefix);
}

std::string_view prefix() {
  std::lock_guard lock(g_mutex);
  return g_prefix;
}

bool flag(std::string_view name) {
  std::lock_guard lock(g_mutex);
  const std::string& v = cached(name);
  if (!g_present[full_name(name)]) return false;
  return !falsey(v);
}

long number(std::string_view name, long fallback) {
  std::lock_guard lock(g_mutex);
  const std::string& v = cached(name);
  if (v.empty()) return fallback;
  char* end = nullptr;
  const long parsed = std::strtol(v.c_str(), &end, 0);  // base 0: decimal, 0x hex, 0 octal
  return (end && end != v.c_str()) ? parsed : fallback;
}

const std::string& text(std::string_view name) {
  std::lock_guard lock(g_mutex);
  return cached(name);
}

bool present(std::string_view name) {
  std::lock_guard lock(g_mutex);
  cached(name);
  return g_present[full_name(name)];
}

std::vector<std::string> active() {
  std::lock_guard lock(g_mutex);
  std::vector<std::string> out;
  if (!environ) return out;
  for (char** e = environ; *e; ++e) {
    std::string_view entry(*e);
    if (g_prefix.empty() || entry.substr(0, g_prefix.size()) == g_prefix) out.emplace_back(entry);
  }
  std::sort(out.begin(), out.end());
  return out;
}

void reset_cache() {
  std::lock_guard lock(g_mutex);
  g_values.clear();
  g_present.clear();
}

}  // namespace lucent::config

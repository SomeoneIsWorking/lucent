#include "lucent/cvar.hpp"

#include "lucent/config.h"
#include "lucent/cvar_c.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

namespace lucent::cvar {

// ── value parsing / formatting ─────────────────────────────────────────────────
namespace detail {
namespace {

std::string lower(std::string_view v) {
  std::string out(v);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string_view trim(std::string_view v) {
  const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!v.empty() && is_space(static_cast<unsigned char>(v.front())))
    v.remove_prefix(1);
  while (!v.empty() && is_space(static_cast<unsigned char>(v.back())))
    v.remove_suffix(1);
  return v;
}

} // namespace

bool parse(std::string_view text, bool &out) {
  const std::string t = lower(trim(text));
  if (t == "1" || t == "true" || t == "yes" || t == "on") {
    out = true;
    return true;
  }
  if (t == "0" || t == "false" || t == "no" || t == "off" || t.empty()) {
    out = false;
    return true;
  }
  return false;
}

bool parse(std::string_view text, long &out) {
  const std::string t(trim(text));
  if (t.empty())
    return false;
  char *end = nullptr;
  const long v = std::strtol(t.c_str(), &end, 0);
  if (end == nullptr || *end != '\0')
    return false;
  out = v;
  return true;
}

bool parse(std::string_view text, double &out) {
  const std::string t(trim(text));
  if (t.empty())
    return false;
  char *end = nullptr;
  const double v = std::strtod(t.c_str(), &end);
  if (end == nullptr || *end != '\0')
    return false;
  out = v;
  return true;
}

bool parse(std::string_view text, std::string &out) {
  out.assign(trim(text));
  return true;
}

std::string format(bool value) {
  return value ? "true" : "false";
}
std::string format(long value) {
  return std::to_string(value);
}
std::string format(double value) {
  std::string s = std::to_string(value);
  // Trim a trailing run of zeros (and a bare '.') from the fixed notation
  // std::to_string produces, so a round-trip does not grow "1" into "1.000000".
  if (s.find('.') != std::string::npos) {
    while (s.size() > 1 && s.back() == '0')
      s.pop_back();
    if (s.back() == '.')
      s.pop_back();
  }
  return s;
}
std::string format(const std::string &value) {
  return value;
}

void report_parse_failure(const std::string &name, std::string_view text) {
  std::fprintf(stderr,
               "lucent::cvar: '%.*s' is not a valid value for '%s'; keeping the previous one\n",
               static_cast<int>(text.size()), text.data(), name.c_str());
}

} // namespace detail

// ── registry state ────────────────────────────────────────────────────────────
namespace {

struct State {
  std::mutex mutex;
  std::string prefix;
  std::unordered_map<std::string, VarBase *> registered;
  // A file value / an explicit override seen before its CVar registered.
  std::unordered_map<std::string, std::string> stashed_value;
  std::unordered_map<std::string, std::string> stashed_override;
  // File keys with no CVar in THIS binary — kept so save_file() writes them back
  // rather than dropping a setting a newer build understands. Ordered for a
  // stable file. A key is erased from here once a CVar for it registers.
  std::map<std::string, std::string> preserved_unknowns;
};

// Constructed on first use and never destroyed — a CVar is a global whose
// constructor may run before this file's, and reset happens at static
// destruction; see the identical rationale in config.cpp.
State &state() {
  static State *s = new State();
  return *s;
}

std::string env_name_locked(const State &st, std::string_view name) {
  std::string out;
  out.reserve(st.prefix.size() + name.size());
  out.append(st.prefix);
  for (char c : name) {
    if (c == '.' || c == '-')
      out.push_back('_');
    else
      out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  return out;
}

// Apply everything below the constructor default that is waiting for `var`:
// a stashed file value, then a stashed --set, then the environment. Called
// with the lock held.
void apply_pending_locked(State &st, VarBase &var) {
  const std::string &name = var.name();
  if (auto it = st.stashed_value.find(name); it != st.stashed_value.end()) {
    var.apply(it->second, Layer::Value);
    st.stashed_value.erase(it);
  }
  st.preserved_unknowns.erase(name);

  const std::string env = env_name_locked(st, name);
  if (lucent::config::present(env))
    var.apply(lucent::config::text(env), Layer::Override);

  if (auto it = st.stashed_override.find(name); it != st.stashed_override.end()) {
    var.apply(it->second, Layer::Override);
    st.stashed_override.erase(it);
  }
}

} // namespace

// ── VarBase ───────────────────────────────────────────────────────────────────
VarBase::VarBase(std::string name) : name_(std::move(name)) {
}
VarBase::~VarBase() = default;

// ── free functions ────────────────────────────────────────────────────────────
void set_prefix(std::string_view prefix) {
  State &st = state();
  std::lock_guard lock(st.mutex);
  st.prefix.assign(prefix);
}

std::string_view prefix() {
  State &st = state();
  std::lock_guard lock(st.mutex);
  return st.prefix;
}

void register_var(VarBase &var) {
  State &st = state();
  std::lock_guard lock(st.mutex);
  auto [it, inserted] = st.registered.emplace(var.name(), &var);
  if (!inserted && it->second != &var) {
    std::fprintf(stderr, "lucent::cvar: two CVars are both named '%s'\n", var.name().c_str());
    std::abort();
  }
  var.registered_ = true;
  apply_pending_locked(st, var);
}

void unregister_var(VarBase &var) {
  State &st = state();
  std::lock_guard lock(st.mutex);
  // Keep its value so save_file() still writes it and a later register restores it.
  if (var.layer() != Layer::Default)
    st.stashed_value[var.name()] = var.dump_persistable();
  st.registered.erase(var.name());
  var.registered_ = false;
}

VarBase *find(std::string_view name) {
  State &st = state();
  std::lock_guard lock(st.mutex);
  const auto it = st.registered.find(std::string(name));
  return it == st.registered.end() ? nullptr : it->second;
}

void enumerate(const std::function<void(VarBase &)> &fn) {
  State &st = state();
  std::lock_guard lock(st.mutex);
  for (auto &[name, var] : st.registered)
    fn(*var);
}

void load_file(const char *path) {
  State &st = state();
  std::lock_guard lock(st.mutex);
  std::ifstream in(path);
  if (!in)
    return; // a missing runtime config is normal; defaults stand.
  std::string line;
  while (std::getline(in, line)) {
    std::string_view view = detail::trim(line);
    if (view.empty() || view.front() == '#')
      continue;
    const auto eq = view.find('=');
    if (eq == std::string_view::npos) {
      std::fprintf(stderr, "lucent::cvar: %s: ignoring line without '=': %.*s\n", path,
                   static_cast<int>(view.size()), view.data());
      continue;
    }
    const std::string key(detail::trim(view.substr(0, eq)));
    const std::string value(detail::trim(view.substr(eq + 1)));
    if (key.empty())
      continue;
    if (const auto it = st.registered.find(key); it != st.registered.end())
      it->second->apply(value, Layer::Value);
    else {
      st.stashed_value[key] = value;
      st.preserved_unknowns[key] = value;
    }
  }
}

bool save_file(const char *path) {
  State &st = state();
  std::lock_guard lock(st.mutex);
  const std::string tmp = std::string(path) + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out)
      return false;
    out << "# lucent::cvar runtime configuration. Layers: default < this file < "
           "environment < --set.\n";
    std::map<std::string, std::string> rows;
    for (const auto &[name, var] : st.registered)
      rows[name] = var->dump_persistable();
    for (const auto &[name, value] : st.preserved_unknowns)
      rows.emplace(name, value); // registered wins if somehow both
    for (const auto &[name, value] : rows)
      out << name << " = " << value << '\n';
    if (!out)
      return false;
  }
  return std::rename(tmp.c_str(), path) == 0;
}

void set_arg(std::string_view name, std::string_view value) {
  State &st = state();
  std::lock_guard lock(st.mutex);
  const std::string key(name);
  if (const auto it = st.registered.find(key); it != st.registered.end())
    it->second->apply(value, Layer::Override);
  else
    st.stashed_override[key] = std::string(value);
}

void reset_for_test() {
  State &st = state();
  std::lock_guard lock(st.mutex);
  st.prefix.clear();
  st.stashed_value.clear();
  st.stashed_override.clear();
  st.preserved_unknowns.clear();
  st.registered.clear();
}

} // namespace lucent::cvar

// ── C ABI ─────────────────────────────────────────────────────────────────────
using lucent::cvar::VarBase;

namespace {

VarBase &require(const char *name) {
  VarBase *var = lucent::cvar::find(name ? name : "");
  if (!var) {
    std::fprintf(stderr, "lucent_cvar: '%s' is not a registered CVar\n", name ? name : "(null)");
    std::abort();
  }
  return *var;
}

} // namespace

extern "C" int lucent_cvar_flag(const char *name, int fallback) {
  bool out = fallback != 0;
  return lucent::cvar::detail::parse(require(name).effective_string(), out) ? (out ? 1 : 0)
                                                                            : (fallback != 0);
}

extern "C" long lucent_cvar_number(const char *name, long fallback) {
  long out = fallback;
  return lucent::cvar::detail::parse(require(name).effective_string(), out) ? out : fallback;
}

extern "C" const char *lucent_cvar_text(const char *name) {
  return require(name).effective_string().c_str();
}

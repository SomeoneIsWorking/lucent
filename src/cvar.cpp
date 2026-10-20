#include "lucent/cvar.hpp"

#include "lucent/config.h"
#include "lucent/cvar_c.h"
#include "lucent/log.h"
#include "lucent/text.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif

namespace lucent::cvar {

// ── value parsing / formatting ─────────────────────────────────────────────────
namespace detail {

bool parse(std::string_view text, bool &out) {
  const std::string t = lucent::text::ascii_lower(lucent::text::trim(text));
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
  const std::string t(lucent::text::trim(text));
  if (t.empty()) {
    return false;
  }
  char *end = nullptr;
  const long v = std::strtol(t.c_str(), &end, 0);
  if (end == nullptr || *end != '\0') {
    return false;
  }
  out = v;
  return true;
}

bool parse(std::string_view text, double &out) {
  const std::string t(lucent::text::trim(text));
  if (t.empty()) {
    return false;
  }
  char *end = nullptr;
  const double v = std::strtod(t.c_str(), &end);
  if (end == nullptr || *end != '\0') {
    return false;
  }
  out = v;
  return true;
}

bool parse(std::string_view text, std::string &out) {
  out.assign(lucent::text::trim(text));
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
    while (s.size() > 1 && s.back() == '0') {
      s.pop_back();
    }
    if (s.back() == '.') {
      s.pop_back();
    }
  }
  return s;
}
std::string format(const std::string &value) {
  return value;
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
// destruction; see the identical rationale in config.cpp. Constant initialization
// keeps the once flag ready before any cross-translation-unit constructor calls state().
constinit std::once_flag g_state_once;
constinit State *g_state = nullptr;

State &state() {
  std::call_once(g_state_once, [] {
    g_state = new State();
  });
  return *g_state;
}

std::string derived_env_name_locked(const State &st, std::string_view name) {
  std::string out;
  out.reserve(st.prefix.size() + name.size());
  out.append(st.prefix);
  for (char c : name) {
    if (c == '.' || c == '-') {
      out.push_back('_');
    } else {
      out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
  }
  return out;
}

void apply_reporting(VarBase &var, std::string_view text, Layer at, std::string_view source) {
  if (!var.apply(text, at)) {
    lucent::log(lucent::Level::Warn, "cvar",
                std::string(source) + ": \"" + std::string(text) + "\" is not a valid value for " +
                    var.name() + "; ignored");
  }
}

// Apply a stashed file value, then the environment, then a stashed set_arg(). Lock held.
void apply_pending_locked(State &st, VarBase &var) {
  const std::string &name = var.name();
  if (auto it = st.stashed_value.find(name); it != st.stashed_value.end()) {
    if (var.persistable()) {
      apply_reporting(var, it->second, Layer::Value, "config file");
    }
    st.stashed_value.erase(it);
  }
  st.preserved_unknowns.erase(name);

  if (lucent::config::present(var.env_name())) {
    apply_reporting(var, lucent::config::text(var.env_name()), Layer::Override, var.env_name());
  }

  if (auto it = st.stashed_override.find(name); it != st.stashed_override.end()) {
    apply_reporting(var, it->second, Layer::Override, "--set");
    st.stashed_override.erase(it);
  }
}

} // namespace

// ── VarBase ───────────────────────────────────────────────────────────────────
VarBase::VarBase(std::string name, Options options)
    : name_(std::move(name)), options_(std::move(options)) {
}
VarBase::~VarBase() = default;

Layer VarBase::layer() const noexcept {
  for (std::size_t index = kLayerCount - 1; index > 0; --index) {
    if (present_[index]) {
      return static_cast<Layer>(index);
    }
  }
  return Layer::Default;
}

void VarBase::clear(Layer at) noexcept {
  if (at != Layer::Default) {
    present_[static_cast<std::size_t>(at)] = false;
  }
}

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
    lucent::log(lucent::Level::Error, "cvar", "two CVars are both named '" + var.name() + "'");
    std::abort();
  }
  var.registered_ = true;
  var.env_name_ = var.options_.env_name.empty() ? derived_env_name_locked(st, var.name())
                                                : var.options_.env_name;
  apply_pending_locked(st, var);
}

void unregister_var(VarBase &var) {
  State &st = state();
  std::lock_guard lock(st.mutex);
  // Keep its value so save_file() still writes it and a later register restores it.
  if (var.persistable() && var.has(Layer::Value)) {
    st.stashed_value[var.name()] = var.dump_persistable();
  }
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
  for (auto &[name, var] : st.registered) {
    fn(*var);
  }
}

void load_file(const char *path) {
  State &st = state();
  std::lock_guard lock(st.mutex);
  std::ifstream in(path);
  if (!in) {
    return; // a missing runtime config is normal; defaults stand.
  }
  std::string line;
  while (std::getline(in, line)) {
    std::string_view view = lucent::text::trim(line);
    if (view.empty() || view.front() == '#') {
      continue;
    }
    const auto eq = view.find('=');
    if (eq == std::string_view::npos) {
      lucent::log(lucent::Level::Warn, "cvar",
                  std::string(path) + ": ignoring line without '=': " + std::string(view));
      continue;
    }
    const std::string key(lucent::text::trim(view.substr(0, eq)));
    const std::string value(lucent::text::trim(view.substr(eq + 1)));
    if (key.empty()) {
      continue;
    }
    if (const auto it = st.registered.find(key); it != st.registered.end()) {
      if (it->second->persistable()) {
        apply_reporting(*it->second, value, Layer::Value, path);
      } else {
        lucent::log(lucent::Level::Warn, "cvar",
                    std::string(path) + ": " + key + " is run-only; ignored");
      }
    } else {
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
    if (!out) {
      return false;
    }
    out << "# lucent::cvar runtime configuration. Layers: default < this file < "
           "environment < --set.\n";
    std::map<std::string, std::string> rows;
    for (const auto &[name, var] : st.registered) {
      if (var->persistable()) {
        rows[name] = var->dump_persistable();
      }
    }
    for (const auto &[name, value] : st.preserved_unknowns) {
      rows.emplace(name, value); // registered wins if somehow both
    }
    for (const auto &[name, value] : rows) {
      out << name << " = " << value << '\n';
    }
    if (!out) {
      return false;
    }
  }
#ifdef _WIN32
  return MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
  return std::rename(tmp.c_str(), path) == 0;
#endif
}

// The stable API accepts two string views for a name/value override token.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void set_arg(std::string_view name, std::string_view value) {
  State &st = state();
  std::lock_guard lock(st.mutex);
  const std::string key(name);
  if (const auto it = st.registered.find(key); it != st.registered.end()) {
    apply_reporting(*it->second, value, Layer::Override, "--set");
  } else {
    st.stashed_override[key] = std::string(value);
  }
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
    lucent::log(lucent::Level::Error, "cvar",
                std::string{"'"} + (name ? name : "(null)") + "' is not a registered CVar");
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

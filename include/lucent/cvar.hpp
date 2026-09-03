// lucent/cvar.hpp — a layered configuration variable ("CVar").
//
// This is the heavier sibling of lucent/config.h. config.h is the env-only
// primitive the logger resolves before main(); cvar builds ON it and adds the
// two layers a program actually configures from:
//
//     compiled default  <  config file  <  environment  <  explicit override
//     (Var constructor)    (load_file)     (config.h)      (set_arg / --set)
//
// A CVar is a global object of type Var<T>. Register it once at startup, then
// read it anywhere:
//
//     lucent::cvar::Var<std::string> g_engine{"engine", "jit"};
//     lucent::cvar::Var<bool>        g_jit_cache{"jit.cache", true};
//
//     lucent::cvar::set_prefix("X2_");     // "jit.cache" then reads X2_JIT_CACHE
//     lucent::cvar::register_var(g_engine);
//     lucent::cvar::register_var(g_jit_cache);
//     lucent::cvar::load_file(path);       // may run before or after register_var
//     lucent::cvar::set_arg("jit.cache", "off");   // one --set token
//
//     if (!g_jit_cache.get()) ...
//
// Modelled on ~/repo/dusklight's ConfigVar system (src/dusk/config_var.hpp).
// Two Dusklight features are deliberately left out for now, each a one-place
// extension: the Speedrun layer (title-specific) and change subscriptions
// (xmen2's consumers read once at init). See the notes at their call sites.
#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace lucent::cvar {

// Where a CVar's effective value is currently coming from. Higher wins.
enum class Layer : unsigned char {
  Default,  // the constructor argument
  Value,    // a value read from the config file; the layer save_file() writes
  Override, // environment or an explicit set_arg(); never persisted
};

// Non-template base so the registry can hold every CVar and the file/arg
// loaders can push strings into one without knowing its type.
class VarBase {
public:
  VarBase(const VarBase &) = delete;
  VarBase &operator=(const VarBase &) = delete;
  virtual ~VarBase();

  [[nodiscard]] const std::string &name() const noexcept { return name_; }
  [[nodiscard]] Layer layer() const noexcept { return layer_; }
  [[nodiscard]] bool registered() const noexcept { return registered_; }

  // Parse `text` and apply it at the named layer. A parse failure is reported
  // to stderr and leaves the CVar unchanged — a bad line in a config file must
  // not silently reset a setting, and it must not abort the program either.
  virtual void apply(std::string_view text, Layer at) = 0;

  // The current effective value, formatted for save_file(). Reads the Value
  // layer (or the Default when no file/override touched it) — never a
  // non-persistable Override.
  [[nodiscard]] virtual std::string dump_persistable() const = 0;

  // The current effective value as a string, INCLUDING an active Override. The
  // returned reference is backed by an internal cache and stays valid until the
  // next call on this CVar; it is how the C ABI (cvar_c.h) reads a value.
  [[nodiscard]] virtual const std::string &effective_string() const = 0;

protected:
  VarBase(std::string name);

  std::string name_;
  Layer layer_ = Layer::Default;
  bool registered_ = false;

  friend void register_var(VarBase &);
  friend void unregister_var(VarBase &);
};

namespace detail {
bool parse(std::string_view text, bool &out);
bool parse(std::string_view text, long &out);
bool parse(std::string_view text, double &out);
bool parse(std::string_view text, std::string &out);
std::string format(bool value);
std::string format(long value);
std::string format(double value);
std::string format(const std::string &value);
void report_parse_failure(const std::string &name, std::string_view text);
} // namespace detail

template <class T> class Var final : public VarBase {
public:
  explicit Var(std::string name, T default_value = T{})
      : VarBase(std::move(name)), default_(std::move(default_value)), value_(default_),
        override_(default_) {}

  // Not guaranteed to stay current across a later mutation, but always sound.
  [[nodiscard]] const T &get() const noexcept {
    switch (layer_) {
    case Layer::Default:
      return default_;
    case Layer::Value:
      return value_;
    case Layer::Override:
      return override_;
    }
    return default_;
  }
  operator const T &() const noexcept { return get(); }
  [[nodiscard]] const T &default_value() const noexcept { return default_; }

  // Runtime change by the program (a settings screen). Stored at the Value
  // layer; save_file() will persist it. Does not disturb an active Override.
  void set(T v) {
    value_ = std::move(v);
    if (layer_ != Layer::Override)
      layer_ = Layer::Value;
  }

  void apply(std::string_view text, Layer at) override {
    T parsed{};
    if (!detail::parse(text, parsed)) {
      detail::report_parse_failure(name_, text);
      return;
    }
    if (at == Layer::Override) {
      override_ = std::move(parsed);
      layer_ = Layer::Override;
    } else {
      value_ = std::move(parsed);
      if (layer_ != Layer::Override)
        layer_ = Layer::Value;
    }
  }

  [[nodiscard]] std::string dump_persistable() const override {
    return detail::format(layer_ == Layer::Default ? default_ : value_);
  }

  [[nodiscard]] const std::string &effective_string() const override {
    str_cache_ = detail::format(get());
    return str_cache_;
  }

private:
  T default_;
  T value_;
  T override_;
  mutable std::string str_cache_;
};

// The prefix applied to a CVar's name before it is looked up in the
// environment: "jit.cache" with prefix "X2_" reads X2_JIT_CACHE ('.' and '-'
// become '_', the rest upper-cased). Set once at startup, before register_var.
void set_prefix(std::string_view prefix);
[[nodiscard]] std::string_view prefix();

// Make the config system aware of a CVar. Applies, in order: any value and any
// override that a load_file() / set_arg() call already stashed for this name,
// then the environment (via lucent::config) at the Override layer.
void register_var(VarBase &var);

// Detach a CVar (so a Var can be destroyed). Its persistable value is kept as an
// unregistered key: save_file() still writes it and a later register_var()
// restores it.
void unregister_var(VarBase &var);

// Look up a registered CVar by name. Null if there is none.
[[nodiscard]] VarBase *find(std::string_view name);

// Call `fn` for every registered CVar.
void enumerate(const std::function<void(VarBase &)> &fn);

// Load `name = value` lines (‘#’ starts a comment, surrounding space ignored)
// at the Value layer. A key with no registered CVar yet is stashed and applied
// when that CVar registers. Keys not recognised at save time are written back
// verbatim so an older binary cannot drop a newer build's settings.
void load_file(const char *path);

// Write every registered CVar's persistable value plus every preserved unknown
// key to `path`. Returns false (and leaves any existing file untouched) on an
// I/O error.
bool save_file(const char *path);

// Apply one explicit override, e.g. from a `--set name=value` launch argument.
// Highest layer; wins over the environment. Stashed if `name` is not registered.
void set_arg(std::string_view name, std::string_view value);

// Forget stashed keys and the prefix. For tests.
void reset_for_test();

} // namespace lucent::cvar

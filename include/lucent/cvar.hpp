// lucent/cvar.hpp — layered configuration variables ("CVars").
//
//     compiled default  <  config file  <  environment / --set  <  runtime
//     (Var constructor)    (load_file)     (register_var, set_arg)  (set at Layer::Runtime)
//
//     lucent::cvar::Var<bool> g_jit_cache{"jit.cache", true};
//     lucent::cvar::set_prefix("X2_");     // "jit.cache" reads X2_JIT_CACHE
//     lucent::cvar::register_var(g_jit_cache);
//     lucent::cvar::load_file(path);
//     if (!g_jit_cache.get()) ...
#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace lucent::cvar {

// Where a value comes from. Higher wins.
enum class Layer : unsigned char {
  Default,  // the constructor argument
  Value,    // the config file; the only layer save_file() writes
  Override, // environment or set_arg()
  Runtime,  // set by the running program for this run
};
inline constexpr std::size_t kLayerCount = 4;

enum class Persistence : unsigned char {
  Saved,   // load_file() fills the Value layer and save_file() writes it
  RunOnly, // never read from or written to the config file
};

struct Options {
  Persistence persistence = Persistence::Saved;
  // Environment variable read verbatim instead of the prefixed, upper-cased name.
  std::string env_name;
};

// Non-template base so the registry and the loaders can hold every CVar.
class VarBase {
public:
  VarBase(const VarBase &) = delete;
  VarBase &operator=(const VarBase &) = delete;
  virtual ~VarBase();

  [[nodiscard]] const std::string &name() const noexcept {
    return name_;
  }
  [[nodiscard]] bool persistable() const noexcept {
    return options_.persistence == Persistence::Saved;
  }
  [[nodiscard]] bool registered() const noexcept {
    return registered_;
  }
  // The environment variable this CVar reads; empty until registered.
  [[nodiscard]] const std::string &env_name() const noexcept {
    return env_name_;
  }

  // The highest layer holding a value.
  [[nodiscard]] Layer layer() const noexcept;
  [[nodiscard]] bool has(Layer at) const noexcept {
    return present_[static_cast<std::size_t>(at)];
  }

  // Parse `text` and store it at `at`. On a parse failure nothing changes and false is returned.
  virtual bool apply(std::string_view text, Layer at) = 0;
  // Drop the value at `at`; the Default layer cannot be cleared.
  void clear(Layer at) noexcept;

  // The value stored at `at`, formatted; empty when that layer holds none.
  [[nodiscard]] virtual std::string layer_string(Layer at) const = 0;
  // The Value layer, else the Default: what save_file() writes.
  [[nodiscard]] std::string dump_persistable() const {
    return layer_string(has(Layer::Value) ? Layer::Value : Layer::Default);
  }
  // The effective value, formatted. Valid until the next call on this CVar.
  [[nodiscard]] virtual const std::string &effective_string() const = 0;

protected:
  VarBase(std::string name, Options options);

  std::string name_;
  Options options_;
  std::string env_name_;
  std::array<bool, kLayerCount> present_{true, false, false, false};
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
} // namespace detail

template <class T> class Var final : public VarBase {
public:
  explicit Var(std::string name, T default_value = T{}, Options options = {})
      : VarBase(std::move(name), std::move(options)) {
    slots_[0] = std::move(default_value);
  }

  [[nodiscard]] const T &get() const noexcept {
    return slots_[static_cast<std::size_t>(layer())];
  }
  operator const T &() const noexcept {
    return get();
  }
  [[nodiscard]] const T &default_value() const noexcept {
    return slots_[0];
  }
  // The value at `at`, or null when that layer holds none.
  [[nodiscard]] const T *at(Layer layer) const noexcept {
    return has(layer) ? &slots_[static_cast<std::size_t>(layer)] : nullptr;
  }

  void set(T value, Layer at = Layer::Value) {
    const auto index = static_cast<std::size_t>(at);
    slots_[index] = std::move(value);
    present_[index] = true;
  }

  bool apply(std::string_view text, Layer at) override {
    T parsed{};
    if (!detail::parse(text, parsed)) {
      return false;
    }
    set(std::move(parsed), at);
    return true;
  }

  [[nodiscard]] std::string layer_string(Layer at) const override {
    return has(at) ? detail::format(slots_[static_cast<std::size_t>(at)]) : std::string();
  }

  [[nodiscard]] const std::string &effective_string() const override {
    str_cache_ = detail::format(get());
    return str_cache_;
  }

private:
  std::array<T, kLayerCount> slots_{};
  mutable std::string str_cache_;
};

// Prefix for derived environment names: "jit.cache" with "X2_" reads X2_JIT_CACHE ('.' and '-'
// become '_', letters are upper-cased). Set once at startup, before register_var.
void set_prefix(std::string_view prefix);
[[nodiscard]] std::string_view prefix();

// Make the registry aware of a CVar and apply, in order, a stashed file value, the environment and
// a stashed set_arg(). An environment value that does not parse is logged and ignored.
void register_var(VarBase &var);

// Detach a CVar. A persistable value is kept, so save_file() still writes it and a later
// register_var() restores it.
void unregister_var(VarBase &var);

[[nodiscard]] VarBase *find(std::string_view name);
void enumerate(const std::function<void(VarBase &)> &fn);

// Load `name = value` lines ('#' starts a comment) at the Value layer. A key with no registered
// CVar is stashed for a later register_var() and written back by save_file().
void load_file(const char *path);

// Write every persistable CVar's file value plus every preserved unknown key. False, leaving any
// existing file untouched, on an I/O error.
bool save_file(const char *path);

// Apply one explicit override (a `--set name=value` argument) at the Override layer. Stashed if
// `name` is not registered.
void set_arg(std::string_view name, std::string_view value);

// Forget stashed keys and the prefix.
void reset_for_test();

} // namespace lucent::cvar

#include "lucent/config.h"
#include "lucent/cvar.hpp"
#include "lucent/cvar_c.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

using lucent::cvar::Layer;
using lucent::cvar::Var;

namespace {

int g_temp_counter = 0;

std::string write_temp(const std::string &body) {
  const std::string path = "lucent-cvar-test-" + std::to_string(getpid()) + "-" +
                           std::to_string(g_temp_counter++) + ".conf";
  std::ofstream(path) << body;
  return path;
}

std::string read_file(const std::string &path) {
  std::ifstream in(path);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void defaults_stand_when_nothing_is_configured() {
  lucent::cvar::reset_for_test();
  Var<std::string> engine{"engine", "jit"};
  Var<bool> cache{"jit.cache", true};
  Var<long> budget{"jit.budget", 100000};
  lucent::cvar::register_var(engine);
  lucent::cvar::register_var(cache);
  lucent::cvar::register_var(budget);
  assert(engine.get() == "jit");
  assert(cache.get() == true);
  assert(budget.get() == 100000);
  assert(engine.layer() == Layer::Default);
}

void file_beats_default_env_beats_file_arg_beats_env() {
  lucent::cvar::reset_for_test();
  lucent::cvar::set_prefix("X2_");

  const std::string path = write_temp("# runtime\nengine = interpreter\njit.cache = off\n");
  lucent::cvar::load_file(path.c_str());

  // File-only so far.
  Var<std::string> engine{"engine", "jit"};
  lucent::cvar::register_var(engine);
  assert(engine.get() == "interpreter");
  assert(engine.layer() == Layer::Value);

  // Environment outranks the file.
  setenv("X2_JIT_CACHE", "on", 1);
  lucent::config::reset_cache();
  Var<bool> cache{"jit.cache", true};
  lucent::cvar::register_var(cache);
  assert(cache.get() == true);
  assert(cache.layer() == Layer::Override);

  // An explicit --set outranks the environment.
  lucent::cvar::set_arg("jit.cache", "off");
  assert(cache.get() == false);

  unsetenv("X2_JIT_CACHE");
  std::remove(path.c_str());
}

void set_arg_before_register_is_stashed_and_applied() {
  lucent::cvar::reset_for_test();
  lucent::cvar::set_arg("jit.verify", "on");
  Var<bool> verify{"jit.verify", false};
  lucent::cvar::register_var(verify);
  assert(verify.get() == true);
}

void c_abi_reads_effective_value_and_aborts_on_unknown() {
  lucent::cvar::reset_for_test();
  Var<std::string> engine{"engine", "jit"};
  Var<bool> cache{"jit.cache", true};
  lucent::cvar::register_var(engine);
  lucent::cvar::register_var(cache);
  assert(std::string(lucent_cvar_text("engine")) == "jit");
  assert(lucent_cvar_flag("jit.cache", 0) == 1);
  cache.set(false);
  assert(lucent_cvar_flag("jit.cache", 1) == 0);

  // Unknown name must abort, not return the fallback.
  if (fork() == 0) {
    std::freopen("/dev/null", "w", stderr);
    lucent_cvar_flag("nonexistent", 0);
    _exit(0); // reached only if it did NOT abort
  }
  int status = 0;
  wait(&status);
  assert(WIFSIGNALED(status));
}

void save_round_trips_and_preserves_unknown_keys() {
  lucent::cvar::reset_for_test();
  const std::string path = write_temp("engine = interpreter\nfuture.knob = 7\n");
  lucent::cvar::load_file(path.c_str());

  Var<std::string> engine{"engine", "jit"};
  lucent::cvar::register_var(engine);
  engine.set("substrate");

  assert(lucent::cvar::save_file(path.c_str()));
  const std::string body = read_file(path);
  assert(body.find("engine = substrate") != std::string::npos);
  assert(body.find("future.knob = 7") != std::string::npos); // not dropped
  std::remove(path.c_str());
}

void unregister_keeps_value_for_save_and_later_register() {
  lucent::cvar::reset_for_test();
  {
    Var<long> budget{"jit.budget", 100};
    lucent::cvar::register_var(budget);
    budget.set(42);
    lucent::cvar::unregister_var(budget);
  }
  Var<long> budget2{"jit.budget", 100};
  lucent::cvar::register_var(budget2);
  assert(budget2.get() == 42);
}

void enumerate_sees_every_registered_var() {
  lucent::cvar::reset_for_test();
  Var<bool> a{"a", false};
  Var<bool> b{"b", false};
  lucent::cvar::register_var(a);
  lucent::cvar::register_var(b);
  int count = 0;
  lucent::cvar::enumerate([&](lucent::cvar::VarBase &) { ++count; });
  assert(count == 2);
}

} // namespace

int main() {
  defaults_stand_when_nothing_is_configured();
  file_beats_default_env_beats_file_arg_beats_env();
  set_arg_before_register_is_stashed_and_applied();
  c_abi_reads_effective_value_and_aborts_on_unknown();
  save_round_trips_and_preserves_unknown_keys();
  unregister_keeps_value_for_save_and_later_register();
  enumerate_sees_every_registered_var();
  std::cout << "cvar: layering, C ABI, save/preserve, unregister, enumerate passed\n";
}

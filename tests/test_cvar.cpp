#include "lucent/config.h"
#include "lucent/cvar.hpp"
#include "lucent/cvar_c.h"
#include "test_environment.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

using lucent::cvar::Layer;
using lucent::cvar::Var;

namespace {

int g_temp_counter = 0;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    std::cerr << "FAIL " << __FILE__ << ':' << line << "  " << expression << '\n';
    std::exit(EXIT_FAILURE);
  }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

std::string write_temp(const std::string &body) {
#ifdef _WIN32
  int process_id = _getpid();
#else
  int process_id = getpid();
#endif
  const std::string path = "lucent-cvar-test-" + std::to_string(process_id) + "-" +
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
  CHECK(engine.get() == "jit");
  CHECK(cache.get() == true);
  CHECK(budget.get() == 100000);
  CHECK(engine.layer() == Layer::Default);
}

void file_beats_default_env_beats_file_arg_beats_env() {
  lucent::cvar::reset_for_test();
  lucent::cvar::set_prefix("X2_");

  const std::string path = write_temp("# runtime\nengine = interpreter\njit.cache = off\n");
  lucent::cvar::load_file(path.c_str());

  // File-only so far.
  Var<std::string> engine{"engine", "jit"};
  lucent::cvar::register_var(engine);
  CHECK(engine.get() == "interpreter");
  CHECK(engine.layer() == Layer::Value);

  // Environment outranks the file.
  CHECK(lucent::test::set_environment("X2_JIT_CACHE", "on"));
  lucent::config::reset_cache();
  Var<bool> cache{"jit.cache", true};
  lucent::cvar::register_var(cache);
  CHECK(cache.get() == true);
  CHECK(cache.layer() == Layer::Override);

  // An explicit --set outranks the environment.
  lucent::cvar::set_arg("jit.cache", "off");
  CHECK(cache.get() == false);

  CHECK(lucent::test::unset_environment("X2_JIT_CACHE"));
  std::remove(path.c_str());
}

void set_arg_before_register_is_stashed_and_applied() {
  lucent::cvar::reset_for_test();
  lucent::cvar::set_arg("jit.verify", "on");
  Var<bool> verify{"jit.verify", false};
  lucent::cvar::register_var(verify);
  CHECK(verify.get() == true);
}

void c_abi_reads_effective_value_and_aborts_on_unknown(const char *executable) {
  lucent::cvar::reset_for_test();
  Var<std::string> engine{"engine", "jit"};
  Var<bool> cache{"jit.cache", true};
  lucent::cvar::register_var(engine);
  lucent::cvar::register_var(cache);
  CHECK(std::string(lucent_cvar_text("engine")) == "jit");
  CHECK(lucent_cvar_flag("jit.cache", 0) == 1);
  cache.set(false);
  CHECK(lucent_cvar_flag("jit.cache", 1) == 0);

  // Unknown name must abort, not return the fallback.
#ifdef _WIN32
  std::intptr_t status = _spawnl(_P_WAIT, executable, executable, "--abort-unknown-cvar",
                                 static_cast<const char *>(nullptr));
  CHECK(status == 3);
#else
  (void)executable;
  if (fork() == 0) {
    std::freopen("/dev/null", "w", stderr);
    lucent_cvar_flag("nonexistent", 0);
    _exit(0); // reached only if it did NOT abort
  }
  int status = 0;
  wait(&status);
  CHECK(WIFSIGNALED(status));
#endif
}

void save_round_trips_and_preserves_unknown_keys() {
  lucent::cvar::reset_for_test();
  const std::string path = write_temp("engine = interpreter\nfuture.knob = 7\n");
  lucent::cvar::load_file(path.c_str());

  Var<std::string> engine{"engine", "jit"};
  lucent::cvar::register_var(engine);
  engine.set("substrate");

  CHECK(lucent::cvar::save_file(path.c_str()));
  const std::string body = read_file(path);
  CHECK(body.find("engine = substrate") != std::string::npos);
  CHECK(body.find("future.knob = 7") != std::string::npos); // not dropped
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
  CHECK(budget2.get() == 42);
}

void enumerate_sees_every_registered_var() {
  lucent::cvar::reset_for_test();
  Var<bool> a{"a", false};
  Var<bool> b{"b", false};
  lucent::cvar::register_var(a);
  lucent::cvar::register_var(b);
  int count = 0;
  lucent::cvar::enumerate([&](lucent::cvar::VarBase &) {
    ++count;
  });
  CHECK(count == 2);
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--abort-unknown-cvar") {
#ifdef _WIN32
    // The Debug CRT otherwise opens a modal abort dialog on a headless CI runner.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    lucent_cvar_flag("nonexistent", 0);
    return 0;
  }
  defaults_stand_when_nothing_is_configured();
  file_beats_default_env_beats_file_arg_beats_env();
  set_arg_before_register_is_stashed_and_applied();
  c_abi_reads_effective_value_and_aborts_on_unknown(argv[0]);
  save_round_trips_and_preserves_unknown_keys();
  unregister_keeps_value_for_save_and_later_register();
  enumerate_sees_every_registered_var();
  std::cout << "cvar: layering, C ABI, save/preserve, unregister, enumerate passed\n";
}

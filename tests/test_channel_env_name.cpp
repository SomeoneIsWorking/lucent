// THE TIME-BOMB TEST: a consumer names lucent's channel variable at BUILD time, and the very first
// logging call in the process — one that runs BEFORE main(), with no setup call of any kind having
// executed — must already be gated by that variable.
//
// This is a separate executable from test_lucent.cpp because the thing under test is a compile-time
// definition (LUCENT_CHANNEL_ENV / LUCENT_LOG_FILE_ENV, set on this target in CMakeLists.txt), and
// because "the first logging call in the process" is only meaningful once per process.
//
// THE BUG IT GUARDS. A consumer whose diagnostics variable is not literally LUCENT_DEBUG used to
// have exactly one way to make it work: call lucent::enable_channels() from its own initialisation.
// That works only while something guarantees that initialisation runs before the first log call. In
// the port this came from, ~700 legacy call sites happened to fire during boot and did the loading
// as a side effect; retiring them would have turned every debug channel in four repositories off,
// silently, with nothing in the diagnostics themselves to say why. A mechanism that works by
// accident of ordering is a mechanism that will fail without a message.
//
// The negative this test can print is therefore the interesting one: if the name is NOT honoured, the
// pre-main line is simply absent, so the assertions below check the captured line COUNT and CONTENT,
// and the diagnostic prints what was captured instead — never a bare "no output".
#include "lucent/config.h"
#include "lucent/log.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " << #cond << "\n"; \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

#define CHECK_EQ(a, b)                                                                  \
  do {                                                                                  \
    auto va = (a);                                                                      \
    auto vb = (b);                                                                      \
    if (!(va == vb)) {                                                                  \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " << #a << " == " << #b \
                << "\n  left:  " << va << "\n  right: " << vb << "\n";                  \
      ++g_failures;                                                                     \
    }                                                                                   \
  } while (0)

// ── Everything from here to `g_early_done` runs BEFORE main() ───────────────────────────────────
// Dynamic initialisation within ONE translation unit happens in declaration order, which is what
// makes this expressible at all: the vector exists, then the sink is installed, then the debug call
// happens — and nothing else in the program has run. In particular NOTHING has called set_prefix,
// set_channel_env, enable_channels, or read any configuration.
std::vector<std::string> g_early_lines;

struct InstallEarlySink {
  InstallEarlySink() {
    lucent::set_sink([](lucent::Level, std::string_view line) { g_early_lines.emplace_back(line); });
  }
};
const InstallEarlySink g_early_sink;

// MYAPP_DEBUG=early comes from the ctest ENVIRONMENT property, i.e. it is set before the process
// starts. MYAPP_DEBUG is what this target compiled LUCENT_CHANNEL_ENV to.
const int g_early_done = [] {
  lucent::debug("early", "pre-main line on an enabled channel");
  lucent::debug("quiet", "pre-main line on a channel that is NOT enabled");
  return 0;
}();

void test_the_first_log_call_in_the_process_honours_the_compiled_in_env_name() {
  // The denominator: two debug() calls were made before main, on two DIFFERENT channels, exactly one
  // of which MYAPP_DEBUG names. So this asserts both that the variable was read (the "early" line is
  // present) and that reading it did not simply turn everything on (the "quiet" line is absent).
  std::cerr << "note: captured " << g_early_lines.size() << " pre-main line(s):\n";
  for (const std::string& l : g_early_lines) std::cerr << "  | " << l << "\n";
  if (g_early_lines.empty())
    std::cerr << "note: NOTHING was captured — either the compiled-in channel variable was ignored,"
                 " or MYAPP_DEBUG was not set in this process's environment (it is: '"
              << (std::getenv("MYAPP_DEBUG") ? std::getenv("MYAPP_DEBUG") : "<unset>") << "')\n";

  CHECK_EQ(g_early_lines.size(), std::size_t(1));
  if (g_early_lines.size() == 1)
    CHECK_EQ(g_early_lines[0], std::string("[early] pre-main line on an enabled channel"));
  CHECK_EQ(g_early_done, 0);
}

void test_the_compiled_in_names_are_what_config_reports() {
  CHECK_EQ(lucent::config::channel_env(), std::string("MYAPP_DEBUG"));
  CHECK_EQ(lucent::config::log_file_env(), std::string("MYAPP_LOG_FILE"));
  // A compiled-in name is deliberately IMMUNE to the prefix, so a consumer that also uses a prefix
  // for its ordinary settings cannot accidentally move its own diagnostics variable.
  lucent::config::set_prefix("MYAPP_");
  CHECK_EQ(lucent::config::channel_env(), std::string("MYAPP_DEBUG"));
  lucent::config::set_prefix("");
}

void test_the_environment_is_re_read_when_the_name_changes_late() {
  // The runtime setter is not the defusal — the compiled-in name is — but it must not be a no-op
  // once the set has already been loaded, or it becomes the same silent failure one level down.
  std::vector<std::string> lines;
  lucent::set_sink([&lines](lucent::Level, std::string_view line) { lines.emplace_back(line); });

  setenv("SOME_OTHER_DEBUG", "late", 1);
  lucent::config::set_channel_env("SOME_OTHER_DEBUG");
  lucent::debug("late", "picked up after the set was already loaded");
  lucent::debug("early", "the old variable no longer names this");
  CHECK_EQ(lines.size(), std::size_t(1));
  if (lines.size() == 1)
    CHECK_EQ(lines[0], std::string("[late] picked up after the set was already loaded"));

  lucent::config::set_channel_env("MYAPP_DEBUG");
  lucent::set_sink(nullptr);
}

void test_an_explicit_enable_channels_outranks_a_later_re_read() {
  std::vector<std::string> lines;
  lucent::set_sink([&lines](lucent::Level, std::string_view line) { lines.emplace_back(line); });

  lucent::enable_channels("chosen-in-code");
  lucent::config::set_channel_env("MYAPP_DEBUG");   // would re-read "early" if it won
  lucent::debug("chosen-in-code", "still on");
  lucent::debug("early", "must not come back");
  CHECK_EQ(lines.size(), std::size_t(1));
  if (lines.size() == 1) CHECK_EQ(lines[0], std::string("[chosen-in-code] still on"));

  lucent::enable_channels("");
  lucent::set_sink(nullptr);
}

}  // namespace

int main() {
  test_the_first_log_call_in_the_process_honours_the_compiled_in_env_name();
  test_the_compiled_in_names_are_what_config_reports();
  test_the_environment_is_re_read_when_the_name_changes_late();
  test_an_explicit_enable_channels_outranks_a_later_re_read();

  if (g_failures == 0) std::cout << "all tests passed\n";
  else std::cerr << g_failures << " failure(s)\n";
  return g_failures == 0 ? 0 : 1;
}

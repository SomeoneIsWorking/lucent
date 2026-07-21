// Tests for lucent. No framework — a check macro and a main, so the test binary has the same
// dependency footprint as the library itself (none).
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

#define CHECK_EQ(a, b)                                                                       \
  do {                                                                                       \
    auto va = (a);                                                                           \
    auto vb = (b);                                                                           \
    if (!(va == vb)) {                                                                       \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " << #a << " == " << #b      \
                << "\n  left:  " << va << "\n  right: " << vb << "\n";                       \
      ++g_failures;                                                                          \
    }                                                                                        \
  } while (0)

void set_env(const char* name, const char* value) {
  setenv(name, value, 1);
  lucent::config::reset_cache();
}

// Captures emitted lines so assertions can look at real output rather than eyeballing a terminal.
struct Capture {
  std::vector<std::string> lines;
  Capture() {
    lucent::set_sink([this](lucent::Level, std::string_view line) { lines.emplace_back(line); });
  }
  ~Capture() { lucent::set_sink(nullptr); }
};

void test_config_flag() {
  lucent::config::set_prefix("");
  set_env("LUCENT_TEST_FLAG", "1");
  CHECK(lucent::config::flag("LUCENT_TEST_FLAG"));

  for (const char* falsey : {"0", "false", "FALSE", "no", "off"}) {
    set_env("LUCENT_TEST_FLAG", falsey);
    CHECK(!lucent::config::flag("LUCENT_TEST_FLAG"));
  }
  // Present-but-empty is still "set". flag() collapses it to false; present() does not.
  set_env("LUCENT_TEST_FLAG", "");
  CHECK(lucent::config::present("LUCENT_TEST_FLAG"));

  unsetenv("LUCENT_TEST_ABSENT");
  lucent::config::reset_cache();
  CHECK(!lucent::config::flag("LUCENT_TEST_ABSENT"));
  CHECK(!lucent::config::present("LUCENT_TEST_ABSENT"));
}

void test_config_number_and_text() {
  lucent::config::set_prefix("");
  set_env("LUCENT_TEST_NUM", "42");
  CHECK_EQ(lucent::config::number("LUCENT_TEST_NUM", 7), 42L);
  set_env("LUCENT_TEST_NUM", "0x20");
  CHECK_EQ(lucent::config::number("LUCENT_TEST_NUM", 7), 32L);
  set_env("LUCENT_TEST_NUM", "not-a-number");
  CHECK_EQ(lucent::config::number("LUCENT_TEST_NUM", 7), 7L);
  unsetenv("LUCENT_TEST_NUM");
  lucent::config::reset_cache();
  CHECK_EQ(lucent::config::number("LUCENT_TEST_NUM", 7), 7L);

  set_env("LUCENT_TEST_TEXT", "/tmp/x.log");
  CHECK_EQ(lucent::config::text("LUCENT_TEST_TEXT"), std::string("/tmp/x.log"));
  unsetenv("LUCENT_TEST_TEXT");
  lucent::config::reset_cache();
  CHECK(lucent::config::text("LUCENT_TEST_TEXT").empty());
}

void test_config_prefix() {
  set_env("MYAPP_WIDE", "1");
  lucent::config::set_prefix("MYAPP_");
  CHECK(lucent::config::flag("WIDE"));       // resolves MYAPP_WIDE
  lucent::config::set_prefix("");
  lucent::config::reset_cache();
  CHECK(!lucent::config::flag("WIDE"));      // no bare WIDE in the environment
}

void test_levels_and_prefixing() {
  Capture cap;
  lucent::info("cd", "loaded {} bytes", 12);
  lucent::warn("cd", "odd size {}", 3);
  lucent::error("boot", "cannot open {}", "x.bin");
  CHECK_EQ(cap.lines.size(), std::size_t(3));
  CHECK_EQ(cap.lines[0], std::string("[cd] loaded 12 bytes"));
  CHECK_EQ(cap.lines[1], std::string("[cd:warn] odd size 3"));
  CHECK_EQ(cap.lines[2], std::string("[boot:error] cannot open x.bin"));
}

void test_debug_is_gated() {
  Capture cap;
  lucent::enable_channels("");                 // nothing on
  lucent::debug("gpu", "should not appear");
  CHECK(cap.lines.empty());

  lucent::enable_channel("gpu");
  lucent::debug("gpu", "prim {}", 5);
  lucent::debug("cd", "hidden");
  CHECK_EQ(cap.lines.size(), std::size_t(1));
  CHECK_EQ(cap.lines[0], std::string("[gpu] prim 5"));

  lucent::enable_channels("all");
  lucent::debug("anything", "now visible");
  CHECK_EQ(cap.lines.size(), std::size_t(2));
  lucent::enable_channels("");
}

void test_debug_does_not_evaluate_arguments_when_off() {
  Capture cap;
  lucent::enable_channels("");
  int calls = 0;
  auto expensive = [&calls] { ++calls; return 1; };
  lucent::debug("off-channel", "{}", expensive());
  // The call itself is an ordinary function argument, so it IS evaluated — what we guarantee is that
  // no formatting or output happens. Document the real contract rather than claim a stronger one.
  CHECK_EQ(calls, 1);
  CHECK(cap.lines.empty());
}

void test_leading_newlines_precede_the_tag() {
  Capture cap;
  lucent::info("sbs", "\n\n*** DIVERGENCE ***");
  CHECK_EQ(cap.lines.size(), std::size_t(1));
  CHECK_EQ(cap.lines[0], std::string("\n\n[sbs] *** DIVERGENCE ***"));
}

void test_line_builder() {
  Capture cap;
  lucent::Line row;
  CHECK(row.empty());
  row.add("  {:08x}:", 0x800a6490u);
  for (int b : {0x00, 0x10, 0x20}) row.add(" {:02x}", b);
  CHECK(!row.empty());
  row.flush(lucent::Level::Info, "mem");
  CHECK(row.empty());                                   // flush clears
  CHECK_EQ(cap.lines.size(), std::size_t(1));
  CHECK_EQ(cap.lines[0], std::string("[mem]   800a6490: 00 10 20"));

  // An empty line emits nothing, so a loop that produced no pieces stays silent.
  lucent::Line none;
  none.flush(lucent::Level::Info, "mem");
  CHECK_EQ(cap.lines.size(), std::size_t(1));
}

void test_line_truncates_safely() {
  Capture cap;
  lucent::Line row;
  for (int i = 0; i < 5000; ++i) row.add("{}", 'x');
  row.flush(lucent::Level::Info, "big");
  CHECK_EQ(cap.lines.size(), std::size_t(1));
  CHECK(cap.lines[0].size() <= lucent::Line::kMaxLength + 16);
  CHECK(cap.lines[0].ends_with("..."));
}

void test_line_flush_debug_is_gated() {
  Capture cap;
  lucent::enable_channels("");
  lucent::Line row;
  row.add("hidden");
  row.flush_debug("off");
  CHECK(cap.lines.empty());
  CHECK(row.empty());                                   // still cleared, even when not emitted

  lucent::enable_channel("on");
  lucent::Line row2;
  row2.add("shown");
  row2.flush_debug("on");
  CHECK_EQ(cap.lines.size(), std::size_t(1));
  lucent::enable_channels("");
}

}  // namespace

int main() {
  test_config_flag();
  test_config_number_and_text();
  test_config_prefix();
  test_levels_and_prefixing();
  test_debug_is_gated();
  test_debug_does_not_evaluate_arguments_when_off();
  test_leading_newlines_precede_the_tag();
  test_line_builder();
  test_line_truncates_safely();
  test_line_flush_debug_is_gated();

  if (g_failures == 0) std::cout << "all tests passed\n";
  else std::cerr << g_failures << " failure(s)\n";
  return g_failures == 0 ? 0 : 1;
}

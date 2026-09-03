// Tests for lucent. No framework — a check macro and a main, so the test binary has the same
// dependency footprint as the library itself (none).
#include "lucent/config.h"
#include "lucent/log.h"
#include "lucent/log_c.h"

#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

// THE HARD CASE FOR AN INTERNED HANDLE: a Channel built — and read — during static initialisation,
// before anything has loaded the environment or touched the channel set. Both of these run before
// main(), so whatever caching scheme Channel uses has to survive being primed from a not-yet-loaded
// world and still respond to every later enable_channels().
const lucent::Channel g_early_channel{"early"};
const bool g_early_answer_at_static_init = static_cast<bool>(g_early_channel);

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " << #cond << "\n";                \
      ++g_failures;                                                                                \
    }                                                                                              \
  } while (0)

#define CHECK_EQ(a, b)                                                                             \
  do {                                                                                             \
    auto va = (a);                                                                                 \
    auto vb = (b);                                                                                 \
    if (!(va == vb)) {                                                                             \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " << #a << " == " << #b            \
                << "\n  left:  " << va << "\n  right: " << vb << "\n";                             \
      ++g_failures;                                                                                \
    }                                                                                              \
  } while (0)

void set_env(const char *name, const char *value) {
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

std::string without_timestamp(std::string_view line) {
  std::size_t leading_newlines = 0;
  while (leading_newlines < line.size() && line[leading_newlines] == '\n')
    ++leading_newlines;

  const std::string_view stamped = line.substr(leading_newlines);
  constexpr std::size_t kTimestampLength = 27;
  const bool shape = stamped.size() >= kTimestampLength && stamped[0] == '[' && stamped[5] == '-' &&
                     stamped[8] == '-' && stamped[11] == 'T' && stamped[14] == ':' &&
                     stamped[17] == ':' && stamped[20] == '.' && stamped[24] == 'Z' &&
                     stamped[25] == ']' && stamped[26] == ' ';
  bool digits = shape;
  for (std::size_t i :
       {1u, 2u, 3u, 4u, 6u, 7u, 9u, 10u, 12u, 13u, 15u, 16u, 18u, 19u, 21u, 22u, 23u})
    digits = digits && std::isdigit(static_cast<unsigned char>(stamped[i]));
  CHECK(shape && digits);
  if (!shape || !digits)
    return std::string(line);
  return std::string(leading_newlines, '\n') + std::string(stamped.substr(kTimestampLength));
}

void test_config_flag() {
  lucent::config::set_prefix("");
  set_env("LUCENT_TEST_FLAG", "1");
  CHECK(lucent::config::flag("LUCENT_TEST_FLAG"));

  for (const char *falsey : {"0", "false", "FALSE", "no", "off"}) {
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
  CHECK(lucent::config::flag("WIDE")); // resolves MYAPP_WIDE
  lucent::config::set_prefix("");
  lucent::config::reset_cache();
  CHECK(!lucent::config::flag("WIDE")); // no bare WIDE in the environment
}

void test_levels_and_prefixing() {
  Capture cap;
  lucent::info("cd", "loaded {} bytes", 12);
  lucent::warn("cd", "odd size {}", 3);
  lucent::error("boot", "cannot open {}", "x.bin");
  CHECK_EQ(cap.lines.size(), std::size_t(3));
  CHECK_EQ(without_timestamp(cap.lines[0]), std::string("[cd] loaded 12 bytes"));
  CHECK_EQ(without_timestamp(cap.lines[1]), std::string("[cd:warn] odd size 3"));
  CHECK_EQ(without_timestamp(cap.lines[2]), std::string("[boot:error] cannot open x.bin"));
}

void test_debug_is_gated() {
  Capture cap;
  lucent::enable_channels(""); // nothing on
  lucent::debug("gpu", "should not appear");
  CHECK(cap.lines.empty());

  lucent::enable_channel("gpu");
  lucent::debug("gpu", "prim {}", 5);
  lucent::debug("cd", "hidden");
  CHECK_EQ(cap.lines.size(), std::size_t(1));
  CHECK_EQ(without_timestamp(cap.lines[0]), std::string("[gpu] prim 5"));

  lucent::enable_channels("all");
  lucent::debug("anything", "now visible");
  CHECK_EQ(cap.lines.size(), std::size_t(2));
  lucent::enable_channels("");
}

void test_debug_does_not_evaluate_arguments_when_off() {
  Capture cap;
  lucent::enable_channels("");
  int calls = 0;
  auto expensive = [&calls] {
    ++calls;
    return 1;
  };
  lucent::debug("off-channel", "{}", expensive());
  // The call itself is an ordinary function argument, so it IS evaluated — what we guarantee is
  // that no formatting or output happens. Document the real contract rather than claim a stronger
  // one.
  CHECK_EQ(calls, 1);
  CHECK(cap.lines.empty());
}

void test_leading_newlines_precede_the_tag() {
  Capture cap;
  lucent::info("sbs", "\n\n*** DIVERGENCE ***");
  CHECK_EQ(cap.lines.size(), std::size_t(1));
  CHECK_EQ(without_timestamp(cap.lines[0]), std::string("\n\n[sbs] *** DIVERGENCE ***"));
}

void test_line_builder() {
  Capture cap;
  lucent::Line row;
  CHECK(row.empty());
  row.add("  {:08x}:", 0x800a6490u);
  for (int b : {0x00, 0x10, 0x20})
    row.add(" {:02x}", b);
  CHECK(!row.empty());
  row.flush(lucent::Level::Info, "mem");
  CHECK(row.empty()); // flush clears
  CHECK_EQ(cap.lines.size(), std::size_t(1));
  CHECK_EQ(without_timestamp(cap.lines[0]), std::string("[mem]   800a6490: 00 10 20"));

  // An empty line emits nothing, so a loop that produced no pieces stays silent.
  lucent::Line none;
  none.flush(lucent::Level::Info, "mem");
  CHECK_EQ(cap.lines.size(), std::size_t(1));
}

void test_line_truncates_safely() {
  Capture cap;
  lucent::Line row;
  for (int i = 0; i < 5000; ++i)
    row.add("{}", 'x');
  row.flush(lucent::Level::Info, "big");
  CHECK_EQ(cap.lines.size(), std::size_t(1));
  const std::string emitted = without_timestamp(cap.lines[0]);
  CHECK(emitted.size() <= lucent::Line::kMaxLength + 16);
  CHECK(emitted.ends_with("..."));
}

void test_line_flush_debug_is_gated() {
  Capture cap;
  lucent::enable_channels("");
  lucent::Line row;
  row.add("hidden");
  row.flush_debug("off");
  CHECK(cap.lines.empty());
  CHECK(row.empty()); // still cleared, even when not emitted

  lucent::enable_channel("on");
  lucent::Line row2;
  row2.add("shown");
  row2.flush_debug("on");
  CHECK_EQ(cap.lines.size(), std::size_t(1));
  lucent::enable_channels("");
}

// ── Channel: the interned handle ────────────────────────────────────────────────────────────────

void test_channel_resolved_before_any_config_load() {
  // The value itself depends on the developer's environment (LUCENT_DEBUG may legitimately be set
  // when running the suite), so what is asserted is not the answer but that the handle KEEPS
  // WORKING: primed before load, it must still track every later change of the set.
  std::cerr << "note: pre-load static-init read of Channel{\"early\"} answered "
            << (g_early_answer_at_static_init ? "true" : "false")
            << " (env-dependent, not asserted)\n";
  lucent::enable_channels("");
  CHECK(!g_early_channel);
  lucent::enable_channels("early");
  CHECK(bool(g_early_channel));
  lucent::enable_channels("");
  CHECK(!g_early_channel);
}

void test_channel_flips_when_the_set_changes_after_construction() {
  lucent::enable_channels("");
  const lucent::Channel ch{"otattr"};
  CHECK(!ch); // off, and now cached as off

  lucent::enable_channels("otattr");
  CHECK(bool(ch)); // a stale cache would still say false — the bug this guards

  lucent::enable_channel("otattr", false);
  CHECK(!ch);
  lucent::enable_channel("otattr", true);
  CHECK(bool(ch));

  lucent::enable_channels("");
  CHECK(!ch);
}

void test_channel_honours_all() {
  lucent::enable_channels("");
  const lucent::Channel ch{"never-named-anywhere"};
  CHECK(!ch);
  lucent::enable_channels("all");
  CHECK(bool(ch));
  lucent::enable_channels("");
  CHECK(!ch);
}

void test_channel_that_is_off_stays_off_while_others_are_on() {
  lucent::enable_channels("gpu,cd");
  const lucent::Channel off{"otattr"};
  const lucent::Channel on{"gpu"};
  CHECK(!off);
  CHECK(bool(on));
  CHECK_EQ(std::string(off.name()), std::string("otattr"));
  lucent::enable_channels("");
}

void test_debug_overload_taking_a_channel() {
  Capture cap;
  lucent::enable_channels("");
  const lucent::Channel ch{"otattr"};
  lucent::debug(ch, "store {:08X}", 0x800a6490u);
  CHECK(cap.lines.empty());

  lucent::enable_channels("otattr");
  lucent::debug(ch, "store {:08X}", 0x800a6490u);
  CHECK_EQ(cap.lines.size(), std::size_t(1));
  CHECK_EQ(without_timestamp(cap.lines[0]), std::string("[otattr] store 800A6490"));

  // The other levels take a Channel too, so a migrated call site never has to keep the literal
  // around just to say info().
  lucent::info(ch, "ready");
  CHECK_EQ(without_timestamp(cap.lines.back()), std::string("[otattr] ready"));
  lucent::warn(ch, "odd");
  CHECK_EQ(without_timestamp(cap.lines.back()), std::string("[otattr:warn] odd"));
  lucent::enable_channels("");
}

void test_line_flush_debug_takes_a_channel() {
  Capture cap;
  lucent::enable_channels("");
  const lucent::Channel ch{"rows"};
  lucent::Line row;
  row.add("hidden");
  row.flush_debug(ch);
  CHECK(cap.lines.empty());
  CHECK(row.empty());

  lucent::enable_channels("rows");
  lucent::Line row2;
  row2.add("shown");
  row2.flush_debug(ch);
  CHECK_EQ(cap.lines.size(), std::size_t(1));
  CHECK_EQ(without_timestamp(cap.lines[0]), std::string("[rows] shown"));
  lucent::enable_channels("");
}

// A Channel is read without the lock while another thread changes the set under it. The design says
// a racing reader gets a one-call-stale answer and then re-resolves; what must NEVER happen is a
// reader latching onto one answer permanently. Run this under -fsanitize=thread as well — the
// generation and the packed cache word are relaxed atomics, so a real data race would show up
// there.
void test_channel_tracks_changes_across_threads() {
  lucent::enable_channels("");
  static const lucent::Channel ch{"racy"};
  std::atomic<bool> stop{false};
  std::atomic<int> saw_true{0}, saw_false{0};

  std::vector<std::thread> readers;
  readers.reserve(4);
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        if (ch)
          saw_true.fetch_add(1, std::memory_order_relaxed);
        else
          saw_false.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (int i = 0; i < 200; ++i) {
    lucent::enable_channels(i % 2 ? "racy" : "");
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  stop.store(true, std::memory_order_relaxed);
  for (auto &t : readers)
    t.join();

  // Both answers must have been observed: all-false would mean the handle never noticed an enable,
  // all-true that it never noticed a disable. Either is the stale-cache bug this must not have.
  CHECK(saw_true.load() > 0);
  CHECK(saw_false.load() > 0);
  lucent::enable_channels("");
  CHECK(!ch);
}

// ── The measurement the header's claim has to earn ──────────────────────────────────────────────
// The previous docstring asserted "a disabled channel costs one cached bool test" without ever
// measuring the case that matters — SOME channel enabled, the caller's channel off — where the old
// path took a mutex, built a std::string and hashed it. This benchmark times exactly that case both
// ways and prints real numbers. It is deliberately run with a channel enabled ("gpu") that is NOT
// the channel being tested ("otattr").
volatile unsigned g_bench_sink = 0;

void test_channel_gate_is_measurably_cheaper() {
  lucent::enable_channels("gpu"); // SOME channel on -> the g_any_enabled fast path is off
  const lucent::Channel ch{"otattr"};
  CHECK(!ch);
  CHECK(!lucent::channel_on("otattr"));

  constexpr int kWarm = 10000;
  constexpr int kIters = 2000000;
  using clock = std::chrono::steady_clock;

  unsigned acc = 0;
  for (int i = 0; i < kWarm; ++i) {
    acc += lucent::channel_on("otattr");
    acc += bool(ch);
  }
  g_bench_sink = acc;

  const auto t0 = clock::now();
  for (int i = 0; i < kIters; ++i)
    acc += lucent::channel_on("otattr");
  const auto t1 = clock::now();
  for (int i = 0; i < kIters; ++i)
    acc += bool(ch);
  const auto t2 = clock::now();
  g_bench_sink = acc;

  const double sv_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / kIters;
  const double ch_ns = std::chrono::duration<double, std::nano>(t2 - t1).count() / kIters;

  // The same shape again, but through the actual call site a consumer writes.
  const auto d0 = clock::now();
  for (int i = 0; i < kIters; ++i)
    lucent::debug("otattr", "store {:08X}", unsigned(i));
  const auto d1 = clock::now();
  for (int i = 0; i < kIters; ++i)
    lucent::debug(ch, "store {:08X}", unsigned(i));
  const auto d2 = clock::now();
  const double dsv_ns = std::chrono::duration<double, std::nano>(d1 - d0).count() / kIters;
  const double dch_ns = std::chrono::duration<double, std::nano>(d2 - d1).count() / kIters;

  std::cerr << "bench (channel off, another channel ON), ns per call, " << kIters << " iters:\n"
            << "  channel_on(string_view) " << sv_ns << "\n"
            << "  Channel gate            " << ch_ns << "   (" << (sv_ns / ch_ns) << "x)\n"
            << "  debug(string_view, ...) " << dsv_ns << "\n"
            << "  debug(Channel, ...)     " << dch_ns << "   (" << (dsv_ns / dch_ns) << "x)\n";

  // Keep the measurement visible, but do not turn scheduler noise into a correctness verdict. The
  // identical binary produced 20.5x and 5.7x on consecutive runs, straddling the old 8x threshold;
  // that gate could not distinguish the locked regression it claimed to catch. The deterministic
  // mutex-independence test below owns that verdict now.
  std::cerr << "  ratio string/Channel    " << (sv_ns / ch_ns)
            << "  (was 29x with the locked map)\n";
  lucent::enable_channels("");
}

void test_string_keyed_gate_does_not_wait_for_the_logger_mutex() {
  lucent::enable_channels("gpu");

  std::mutex mutex;
  std::condition_variable changed;
  bool sink_entered = false;
  bool release_sink = false;
  lucent::set_sink([&](lucent::Level, std::string_view) {
    std::unique_lock lock(mutex);
    sink_entered = true;
    changed.notify_all();
    changed.wait(lock, [&] { return release_sink; });
  });

  std::thread writer([] { lucent::info("hold", "logger mutex held by the sink"); });
  {
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return sink_entered; });
  }

  std::promise<void> gate_started;
  auto gate = std::async(std::launch::async, [&] {
    gate_started.set_value();
    return lucent::channel_on("otattr");
  });
  gate_started.get_future().wait();
  const bool completed_without_logger =
      gate.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  CHECK(completed_without_logger);

  {
    std::lock_guard lock(mutex);
    release_sink = true;
  }
  changed.notify_all();
  writer.join();
  CHECK(!gate.get());
  lucent::set_sink(nullptr);
  lucent::enable_channels("");
}

// The snapshot must AGREE with the set it is published from, through every mutation that can change
// it. This is the correctness half of the change above: a stale snapshot would make a channel the
// caller just enabled read as off (silently no logging) or one just disabled read as on. Both forms
// are checked together at each step, because they now share one source of truth and the failure
// that matters is them disagreeing with the set — or with each other.
void test_string_keyed_gate_tracks_the_set() {
  const lucent::Channel ch{"snap"};

  lucent::enable_channels("");
  CHECK(!lucent::channel_on("snap"));
  CHECK(!ch);

  lucent::enable_channel("snap"); // single-channel mutation
  CHECK(lucent::channel_on("snap"));
  CHECK(bool(ch));
  CHECK(!lucent::channel_on("other"));

  lucent::enable_channel("snap", false); // ...and back off
  CHECK(!lucent::channel_on("snap"));
  CHECK(!ch);

  lucent::enable_channels("a,snap,b"); // whole-list mutation
  CHECK(lucent::channel_on("snap"));
  CHECK(lucent::channel_on("a"));
  CHECK(lucent::channel_on("b"));
  CHECK(!lucent::channel_on("c"));
  CHECK(bool(ch));

  lucent::enable_channels("all"); // the wildcard
  CHECK(lucent::channel_on("anything-at-all"));
  CHECK(bool(ch));

  lucent::enable_channels("");
  CHECK(!lucent::channel_on("snap"));
  CHECK(!lucent::channel_on("anything-at-all"));
  CHECK(!ch);
}

void test_format_builds_a_string_without_emitting() {
  // format() is the composition helper behind conditional fragments: it must produce identical
  // text to the level helpers and must NOT touch the sink or any channel state.
  const std::string s = lucent::format("aux=0x{:08X} n={}", 0x1234u, 7);
  CHECK(s == "aux=0x00001234 n=7");

  lucent::enable_channels("quiet");
  const std::string before = lucent::format("plain");
  CHECK(before == "plain");
  lucent::enable_channels("");
}

void test_c_logging_api() {
  Capture cap;
  lucent::enable_channels("cchan");
  lucent_log_info("cchan", "info message %d", 42);
  lucent_log_warn("cchan", "warn message %s", "test");
  lucent_log_error("cchan", "error message");
  lucent_log_debug("cchan", "debug on %d", 1);
  lucent_log_debug("offchan", "debug off %d", 2);
  lucent::enable_channels("");

  CHECK_EQ(cap.lines.size(), 4u);
  if (cap.lines.size() == 4u) {
    CHECK_EQ(without_timestamp(cap.lines[0]), "[cchan] info message 42");
    CHECK_EQ(without_timestamp(cap.lines[1]), "[cchan:warn] warn message test");
    CHECK_EQ(without_timestamp(cap.lines[2]), "[cchan:error] error message");
    CHECK_EQ(without_timestamp(cap.lines[3]), "[cchan] debug on 1");
  }
}

} // namespace

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
  test_channel_resolved_before_any_config_load();
  test_channel_flips_when_the_set_changes_after_construction();
  test_channel_honours_all();
  test_channel_that_is_off_stays_off_while_others_are_on();
  test_debug_overload_taking_a_channel();
  test_line_flush_debug_takes_a_channel();
  test_channel_tracks_changes_across_threads();
  test_channel_gate_is_measurably_cheaper();
  test_string_keyed_gate_does_not_wait_for_the_logger_mutex();
  test_string_keyed_gate_tracks_the_set();
  test_format_builds_a_string_without_emitting();
  test_c_logging_api();

  if (g_failures == 0)
    std::cout << "all tests passed\n";
  else
    std::cerr << g_failures << " failure(s)\n";
  return g_failures == 0 ? 0 : 1;
}

#include "lucent/log.h"

#include "lucent/config.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace lucent {

// Starts at 1, not 0: a Channel's packed cache word is value-initialised to 0, i.e. "generation 0,
// answer false". Starting the global at 1 makes that initial state unmatchable, so the first read
// of any Channel — including one constructed and read during static initialisation, before the
// environment has ever been consulted — necessarily misses and resolves properly. A 0 start would
// have made an early-constructed handle report a confident false forever.
namespace detail {
std::atomic<std::uint64_t> g_channel_generation{1};
} // namespace detail

namespace {

// CONSTRUCTED ON FIRST USE AND NEVER DESTROYED — see the identical note in config.cpp. As plain
// namespace-scope objects these were subject to static-initialisation order across translation
// units, so a consumer logging from its own static initialiser could reach an unconstructed
// container. A logger must work from the program's first line to its last.
struct State {
  std::mutex mutex;
  Sink sink;                                // empty -> default file/stderr sink
  std::FILE *stream = nullptr;              // resolved once, on first use
  std::unordered_set<std::string> channels; // explicitly enabled channels
  bool all_channels = false;
  bool channels_loaded = false;
  // True once enable_channels()/enable_channel() has been called. Code that names its channels
  // outranks the environment, so a later re-arm must not throw that away.
  bool channels_explicit = false;
};

State &state() {
  static State *s = new State();
  return *s;
}

// FAST PATH FOR THE OVERWHELMINGLY COMMON CASE: no channels enabled at all.
//
// channel_enabled() is called at EVERY debug() site, whether or not the channel is on, and it took
// the mutex and constructed a std::string from the string_view before it could answer "no". A
// consuming port measured it at 6.06% of total CPU with logging entirely switched OFF — an
// unconditional cost paid by every caller for the privilege of not logging. That is the wrong shape
// for a library whose whole premise is that a disabled channel is free.
//
// `g_any_enabled` is false whenever the channel set is loaded and empty. It is written only under
// the state mutex and read without it, which is safe here: the read is a single relaxed atomic
// load, and a reader that races a concurrent enable_channels() simply gets the old answer for one
// call — the same outcome it would get by acquiring the lock a moment earlier. Nothing is torn.
//
// It starts FALSE-with-unloaded so the first call still takes the slow path, loads the environment
// and sets it correctly; a bare `false` default would have permanently disabled logging.
std::atomic<bool> g_any_enabled{false};
std::atomic<bool> g_loaded{false};

// THE ENABLED SET, READABLE WITHOUT THE MUTEX — the other half of the fast path above.
//
// `g_any_enabled` made a channel free only while NOTHING was enabled. The moment a consumer turned
// one channel on — which is what anybody debugging or profiling does — every string-keyed call site
// went back to the full slow path: lock the mutex, construct a std::string from the string_view,
// hash it. MEASURED on the Tomba!2 port: enabling a channel that NOTHING logs to (`nosuchchannel`,
// a pure negative control, so zero actual debug work) cost +6.4% of total run time, and the
// in-tree bench below put one string-keyed gate at 18.9 ns against the Channel handle's 0.65 ns.
// So the earlier "a disabled channel is free" fix was true only for the all-off case, and the
// cost it removed came straight back for exactly the runs that measure anything.
//
// The set is READ constantly and WRITTEN almost never (an enable_channels() call or two per
// process), and it holds a handful of short names. That is the shape of an immutable snapshot
// published behind an atomic pointer: readers take no lock, allocate nothing and hash nothing, and
// a lookup is a linear scan of a few string compares that reject on length first.
//
// NEVER FREED, deliberately. Replacing a snapshot leaves the old one alive, so a reader that loaded
// the pointer a moment before a concurrent enable_channels() keeps reading a valid object instead
// of one being destroyed under it — the alternative is reference counting or hazard pointers on the
// hottest read in the library. The leak is bounded by the number of enable calls a process makes
// (a handful), each a few dozen bytes, and it matches this file's existing never-destroyed policy
// for State itself.
struct ChannelSnapshot {
  bool all = false;
  std::vector<std::string> names;
};
std::atomic<const ChannelSnapshot *> g_snapshot{nullptr};

// Publish the current set as a new immutable snapshot. MUST be called under state().mutex, and is
// called from exactly one place (refresh_any_enabled_locked) so no mutation can forget to.
void publish_snapshot_locked() {
  auto *snap = new ChannelSnapshot();
  snap->all = state().all_channels;
  snap->names.assign(state().channels.begin(), state().channels.end());
  // Release: everything written into *snap must be visible to a reader that acquires this pointer.
  g_snapshot.store(snap, std::memory_order_release);
}

// The lock-free answer. A null snapshot means nothing has ever been loaded, which cannot mean
// "enabled" — the callers below load first, so this returns false only when the set is genuinely
// unknown-and-empty.
bool enabled_in_snapshot(std::string_view channel) {
  const ChannelSnapshot *snap = g_snapshot.load(std::memory_order_acquire);
  if (!snap)
    return false;
  if (snap->all)
    return true;
  for (const std::string &name : snap->names)
    if (name == channel)
      return true;
  return false;
}

// Recompute the fast-path flag. MUST be called under state().mutex by anything that changes the
// set.
//
// Also bumps the generation every Channel handle stamps itself against. Anything that can change
// what a channel resolves to comes through here, which is what makes "invalidate every cached
// handle in the program" a single store. Same relaxed-under-the-mutex reasoning as above: a reader
// racing an enable_channels() gets the old answer for one call, then re-resolves.
void refresh_any_enabled_locked() {
  g_any_enabled.store(state().all_channels || !state().channels.empty(), std::memory_order_relaxed);
  g_loaded.store(state().channels_loaded, std::memory_order_relaxed);
  publish_snapshot_locked();
  detail::g_channel_generation.store(
      detail::g_channel_generation.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

// Answer for one name, with the set already loaded. Caller holds state().mutex.
bool channel_enabled_locked(std::string_view channel) {
  // Reads the SAME snapshot the lock-free path does, rather than a second structure that has to be
  // kept in agreement with it. The mutex is already held by the caller, and the snapshot is
  // republished under it by every mutation, so what this sees is current by construction.
  return enabled_in_snapshot(channel);
}

void load_channels_locked() {
  if (state().channels_loaded)
    return;
  state().channels_loaded = true;
  // NOT config::text("LUCENT_DEBUG"): which variable this is comes from config, so that a consumer
  // can name it (PSXPORT_DEBUG) at build time and this lazy first-use load already reads the right
  // one — with no initialisation call for anything to run before.
  const std::string &list = config::channel_list();
  if (list.empty()) {
    // THE CASE THE FAST PATH EXISTS FOR, and the one this early return originally skipped: no
    // channels configured. Without refreshing here, g_loaded stayed false, the lock-free path never
    // engaged, and the optimisation measured as having no effect at all — correct-looking code that
    // did nothing. Set the flags before returning.
    refresh_any_enabled_locked();
    return;
  }
  std::size_t start = 0;
  while (start <= list.size()) {
    const std::size_t comma = list.find(',', start);
    const std::size_t end = (comma == std::string::npos) ? list.size() : comma;
    std::string name = list.substr(start, end - start);
    while (!name.empty() && name.front() == ' ')
      name.erase(name.begin());
    while (!name.empty() && name.back() == ' ')
      name.pop_back();
    if (name == "all")
      state().all_channels = true;
    else if (!name.empty())
      state().channels.insert(std::move(name));
    if (comma == std::string::npos)
      break;
    start = comma + 1;
  }
  refresh_any_enabled_locked();
}

std::FILE *stream_locked() {
  if (state().stream)
    return state().stream;
  const std::string &path = config::log_file_path();
  if (!path.empty()) {
    if (std::FILE *f = std::fopen(path.c_str(), "a")) {
      // Line-buffered so `tail -f` shows progress and a crash does not swallow the last lines.
      std::setvbuf(f, nullptr, _IOLBF, 0);
      state().stream = f;
      return state().stream;
    }
  }
  state().stream = stderr;
  return state().stream;
}

// Warnings and errors suffix the channel so they stay greppable without a separate stream or an
// extra column: "[cd] ok" / "[cd:warn] odd" / "[cd:error] failed".
std::string tag_for(Level level, std::string_view channel) {
  switch (level) {
  case Level::Warn:
    return std::string(channel) + ":warn";
  case Level::Error:
    return std::string(channel) + ":error";
  default:
    return std::string(channel);
  }
}

} // namespace

void log(Level level, std::string_view channel, std::string_view message) {
  // Leading newlines are blank-line separators a caller uses to set a banner apart. Emit them
  // before the prefix, otherwise the tag ends up stranded on the line above its own message.
  std::size_t lead = 0;
  while (lead < message.size() && message[lead] == '\n')
    ++lead;
  const std::string_view body = message.substr(lead);

  std::string line;
  line.reserve(body.size() + channel.size() + 8);
  line.append(lead, '\n');
  line.push_back('[');
  line.append(tag_for(level, channel));
  line.append("] ");
  line.append(body);

  std::lock_guard lock(state().mutex);
  if (state().sink) {
    state().sink(level, line);
    return;
  }
  std::FILE *out = stream_locked();
  std::fwrite(line.data(), 1, line.size(), out);
  std::fputc('\n', out);
}

namespace detail {
bool channel_enabled(std::string_view channel) {
  // No lock, no allocation, no hash: once the set is known to be empty, nothing can be enabled.
  if (g_loaded.load(std::memory_order_relaxed) && !g_any_enabled.load(std::memory_order_relaxed))
    return false;
  // Channels ARE enabled, and this one may not be. Still no lock and no allocation: the snapshot
  // answers it. This is the path that used to cost 18.9 ns and 6.4% of a profiling run.
  if (g_loaded.load(std::memory_order_relaxed))
    return enabled_in_snapshot(channel);
  // First call only: nothing has read the environment yet. Load it, which publishes a snapshot, and
  // then answer from that snapshot like everyone else.
  {
    std::lock_guard lock(state().mutex);
    load_channels_locked();
  }
  return enabled_in_snapshot(channel);
}

void rearm_channels_from_env() {
  std::lock_guard lock(state().mutex);
  if (state().channels_explicit)
    return; // an explicit enable_channels() outranks the environment
  if (!state().channels_loaded)
    return; // nothing loaded yet; the next call reads the new name
  state().channels.clear();
  state().all_channels = false;
  state().channels_loaded = false;
  refresh_any_enabled_locked(); // g_loaded goes false: the fast path re-consults on the
                                // next call, and every Channel handle re-resolves
}
} // namespace detail

bool Channel::resolve() const {
  std::uint64_t gen;
  bool on;
  {
    std::lock_guard lock(state().mutex);
    load_channels_locked();
    on = channel_enabled_locked(name_);
    // Read the generation INSIDE the lock, and after load_channels_locked() — which may itself have
    // bumped it. Stamping with a value read before the lock would label a fresh answer with a
    // superseded generation (harmless) or, worse, label it with one that a concurrent
    // enable_channels() is about to reuse. Under the lock the pair is consistent by construction:
    // if the set changes after we unlock, the generation is already past ours and the next read
    // misses.
    gen = detail::g_channel_generation.load(std::memory_order_relaxed);
  }
  state_.store((gen << 1) | static_cast<std::uint64_t>(on), std::memory_order_relaxed);
  return on;
}

bool channel_on(std::string_view channel) {
  return detail::channel_enabled(channel);
}

void enable_channels(std::string_view list) {
  std::lock_guard lock(state().mutex);
  state().channels.clear();
  state().all_channels = false;
  state().channels_loaded = true;   // an explicit call wins over the environment
  state().channels_explicit = true; // ...and keeps winning across a later re-arm
  std::string text(list);
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t comma = text.find(',', start);
    const std::size_t end = (comma == std::string::npos) ? text.size() : comma;
    std::string name = text.substr(start, end - start);
    while (!name.empty() && name.front() == ' ')
      name.erase(name.begin());
    while (!name.empty() && name.back() == ' ')
      name.pop_back();
    if (name == "all")
      state().all_channels = true;
    else if (!name.empty())
      state().channels.insert(std::move(name));
    if (comma == std::string::npos)
      break;
    start = comma + 1;
  }
  refresh_any_enabled_locked();
}

void enable_channel(std::string_view channel, bool on) {
  std::lock_guard lock(state().mutex);
  load_channels_locked();
  state().channels_explicit = true;
  std::string key(channel);
  if (on)
    state().channels.insert(key);
  else
    state().channels.erase(key);
  refresh_any_enabled_locked();
}

void set_sink(Sink sink) {
  std::lock_guard lock(state().mutex);
  state().sink = std::move(sink);
}

void Line::append(std::string_view piece) {
  if (truncated_)
    return;
  if (text_.size() + piece.size() > kMaxLength) {
    text_.append(piece.substr(0, kMaxLength - text_.size()));
    text_.append("...");
    truncated_ = true;
    return;
  }
  text_.append(piece);
}

void Line::flush(Level level, std::string_view channel) {
  if (text_.empty())
    return;
  log(level, channel, text_);
  clear();
}

void Line::flush_debug(std::string_view channel) {
  if (text_.empty())
    return;
  if (detail::channel_enabled(channel))
    log(Level::Debug, channel, text_);
  clear();
}

void Line::flush_debug(const Channel &channel) {
  if (text_.empty())
    return;
  if (channel.enabled())
    log(Level::Debug, channel.name(), text_);
  clear();
}

} // namespace lucent

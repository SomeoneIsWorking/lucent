#include "lucent/log.h"

#include "lucent/config.h"

#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace lucent {
namespace {

std::mutex g_mutex;
Sink g_sink;                                  // empty -> default file/stderr sink
std::FILE* g_stream = nullptr;                // resolved once, on first use
std::unordered_set<std::string> g_channels;   // explicitly enabled channels
bool g_all_channels = false;
bool g_channels_loaded = false;
std::unordered_map<std::string, bool> g_channel_cache;

void load_channels_locked() {
  if (g_channels_loaded) return;
  g_channels_loaded = true;
  const std::string& list = config::text("LUCENT_DEBUG");
  if (list.empty()) return;
  std::size_t start = 0;
  while (start <= list.size()) {
    const std::size_t comma = list.find(',', start);
    const std::size_t end = (comma == std::string::npos) ? list.size() : comma;
    std::string name = list.substr(start, end - start);
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (name == "all") g_all_channels = true;
    else if (!name.empty()) g_channels.insert(std::move(name));
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
}

std::FILE* stream_locked() {
  if (g_stream) return g_stream;
  const std::string& path = config::text("LUCENT_LOG_FILE");
  if (!path.empty()) {
    if (std::FILE* f = std::fopen(path.c_str(), "a")) {
      // Line-buffered so `tail -f` shows progress and a crash does not swallow the last lines.
      std::setvbuf(f, nullptr, _IOLBF, 0);
      g_stream = f;
      return g_stream;
    }
  }
  g_stream = stderr;
  return g_stream;
}

// Warnings and errors suffix the channel so they stay greppable without a separate stream or an
// extra column: "[cd] ok" / "[cd:warn] odd" / "[cd:error] failed".
std::string tag_for(Level level, std::string_view channel) {
  switch (level) {
    case Level::Warn:  return std::string(channel) + ":warn";
    case Level::Error: return std::string(channel) + ":error";
    default:           return std::string(channel);
  }
}

}  // namespace

void log(Level level, std::string_view channel, std::string_view message) {
  // Leading newlines are blank-line separators a caller uses to set a banner apart. Emit them before
  // the prefix, otherwise the tag ends up stranded on the line above its own message.
  std::size_t lead = 0;
  while (lead < message.size() && message[lead] == '\n') ++lead;
  const std::string_view body = message.substr(lead);

  std::string line;
  line.reserve(body.size() + channel.size() + 8);
  line.append(lead, '\n');
  line.push_back('[');
  line.append(tag_for(level, channel));
  line.append("] ");
  line.append(body);

  std::lock_guard lock(g_mutex);
  if (g_sink) {
    g_sink(level, line);
    return;
  }
  std::FILE* out = stream_locked();
  std::fwrite(line.data(), 1, line.size(), out);
  std::fputc('\n', out);
}

namespace detail {
bool channel_enabled(std::string_view channel) {
  std::lock_guard lock(g_mutex);
  load_channels_locked();
  if (g_all_channels) return true;
  std::string key(channel);
  if (auto it = g_channel_cache.find(key); it != g_channel_cache.end()) return it->second;
  const bool on = g_channels.count(key) != 0;
  g_channel_cache.emplace(std::move(key), on);
  return on;
}
}  // namespace detail

bool channel_on(std::string_view channel) { return detail::channel_enabled(channel); }

void enable_channels(std::string_view list) {
  std::lock_guard lock(g_mutex);
  g_channels.clear();
  g_all_channels = false;
  g_channel_cache.clear();
  g_channels_loaded = true;  // an explicit call wins over the environment
  std::string text(list);
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t comma = text.find(',', start);
    const std::size_t end = (comma == std::string::npos) ? text.size() : comma;
    std::string name = text.substr(start, end - start);
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (name == "all") g_all_channels = true;
    else if (!name.empty()) g_channels.insert(std::move(name));
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
}

void enable_channel(std::string_view channel, bool on) {
  std::lock_guard lock(g_mutex);
  load_channels_locked();
  std::string key(channel);
  if (on) g_channels.insert(key);
  else    g_channels.erase(key);
  g_channel_cache[key] = on;
}

void set_sink(Sink sink) {
  std::lock_guard lock(g_mutex);
  g_sink = std::move(sink);
}

void Line::append(std::string_view piece) {
  if (truncated_) return;
  if (text_.size() + piece.size() > kMaxLength) {
    text_.append(piece.substr(0, kMaxLength - text_.size()));
    text_.append("...");
    truncated_ = true;
    return;
  }
  text_.append(piece);
}

void Line::flush(Level level, std::string_view channel) {
  if (text_.empty()) return;
  log(level, channel, text_);
  clear();
}

void Line::flush_debug(std::string_view channel) {
  if (text_.empty()) return;
  if (detail::channel_enabled(channel)) log(Level::Debug, channel, text_);
  clear();
}

}  // namespace lucent

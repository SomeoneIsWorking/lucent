#include "lucent/log_c.h"
#include "lucent/log.h"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <string_view>
#include <vector>

namespace {
void emit_log(lucent::Level level, const char *channel, const char *fmt, va_list args) {
  va_list measure_args;
  va_copy(measure_args, args);
  std::array<char, 512> buf{};
  int len = std::vsnprintf(buf.data(), buf.size(), fmt, measure_args);
  va_end(measure_args);
  if (len < 0) {
    return;
  }
  if (static_cast<size_t>(len) < buf.size()) {
    lucent::log(level, channel != nullptr ? channel : "",
                std::string_view(buf.data(), static_cast<size_t>(len)));
    return;
  }
  std::vector<char> dyn(static_cast<size_t>(len) + 1);
  va_list render_args;
  va_copy(render_args, args);
  std::vsnprintf(dyn.data(), dyn.size(), fmt, render_args);
  va_end(render_args);
  lucent::log(level, channel != nullptr ? channel : "",
              std::string_view(dyn.data(), static_cast<size_t>(len)));
}
} // namespace

extern "C" {

void lucent_log(LucentLogLevel level, const char *channel, const char *fmt, ...) {
  lucent::Level lvl = lucent::Level::Info;
  switch (level) {
  case LUCENT_LOG_DEBUG:
    if (!lucent::channel_on(channel != nullptr ? channel : ""))
      return;
    lvl = lucent::Level::Debug;
    break;
  case LUCENT_LOG_INFO:
    lvl = lucent::Level::Info;
    break;
  case LUCENT_LOG_WARN:
    lvl = lucent::Level::Warn;
    break;
  case LUCENT_LOG_ERROR:
    lvl = lucent::Level::Error;
    break;
  }
  va_list args;
  va_start(args, fmt);
  emit_log(lvl, channel, fmt, args);
  va_end(args);
}

void lucent_log_debug(const char *channel, const char *fmt, ...) {
  if (!lucent::channel_on(channel != nullptr ? channel : ""))
    return;
  va_list args;
  va_start(args, fmt);
  emit_log(lucent::Level::Debug, channel, fmt, args);
  va_end(args);
}

void lucent_log_info(const char *channel, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  emit_log(lucent::Level::Info, channel, fmt, args);
  va_end(args);
}

void lucent_log_warn(const char *channel, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  emit_log(lucent::Level::Warn, channel, fmt, args);
  va_end(args);
}

void lucent_log_error(const char *channel, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  emit_log(lucent::Level::Error, channel, fmt, args);
  va_end(args);
}

} // extern "C"

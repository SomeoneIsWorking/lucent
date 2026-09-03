#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  LUCENT_LOG_DEBUG = 0,
  LUCENT_LOG_INFO,
  LUCENT_LOG_WARN,
  LUCENT_LOG_ERROR
} LucentLogLevel;

#if defined(__GNUC__) || defined(__clang__)
#define LUCENT_PRINTF_ATTR(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define LUCENT_PRINTF_ATTR(fmt_idx, arg_idx)
#endif

void lucent_log(LucentLogLevel level, const char *channel, const char *fmt, ...)
    LUCENT_PRINTF_ATTR(3, 4);

void lucent_log_debug(const char *channel, const char *fmt, ...) LUCENT_PRINTF_ATTR(2, 3);
void lucent_log_info(const char *channel, const char *fmt, ...) LUCENT_PRINTF_ATTR(2, 3);
void lucent_log_warn(const char *channel, const char *fmt, ...) LUCENT_PRINTF_ATTR(2, 3);
void lucent_log_error(const char *channel, const char *fmt, ...) LUCENT_PRINTF_ATTR(2, 3);

#undef LUCENT_PRINTF_ATTR

#ifdef __cplusplus
}
#endif

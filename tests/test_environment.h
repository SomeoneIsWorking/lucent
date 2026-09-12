#pragma once

#include <cstdlib>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace lucent::test {

inline bool set_environment(const char *name, const char *value) {
#ifdef _WIN32
  return SetEnvironmentVariableA(name, value) != 0;
#else
  return setenv(name, value, 1) == 0;
#endif
}

inline bool unset_environment(const char *name) {
#ifdef _WIN32
  return SetEnvironmentVariableA(name, nullptr) != 0;
#else
  return unsetenv(name) == 0;
#endif
}

} // namespace lucent::test

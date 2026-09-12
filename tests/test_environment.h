#pragma once

#include <cstdlib>

namespace lucent::test {

inline bool set_environment(const char *name, const char *value) {
#ifdef _WIN32
  return _putenv_s(name, value) == 0;
#else
  return setenv(name, value, 1) == 0;
#endif
}

inline bool unset_environment(const char *name) {
#ifdef _WIN32
  return _putenv_s(name, "") == 0;
#else
  return unsetenv(name) == 0;
#endif
}

} // namespace lucent::test

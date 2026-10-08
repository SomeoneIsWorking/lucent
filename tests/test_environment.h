#pragma once

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace lucent::test {

template <typename Body> int run_main(Body &&body) noexcept {
  try {
    return body();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: unhandled test exception: %s\n", error.what());
  } catch (...) {
    std::fputs("FAIL: unhandled non-standard test exception\n", stderr);
  }
  return 1;
}

inline std::string environment_value_or_unset(const char *name) {
#ifdef _WIN32
  char *raw = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&raw, &length, name) != 0) {
    throw std::runtime_error("could not read test environment");
  }
  std::unique_ptr<char, decltype(&std::free)> value(raw, &std::free);
  return value ? std::string(value.get()) : "<unset>";
#else
  const char *value = std::getenv(name);
  return value ? std::string(value) : "<unset>";
#endif
}

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

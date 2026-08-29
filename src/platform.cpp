#include "lucent/platform.h"
#include "lucent/platform_c.h"

#include <cstdlib>
#include <mutex>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace lucent::platform {
namespace {

std::mutex override_mutex;
std::filesystem::path app_private_directory;

bool valid_application_name(std::string_view name) {
  if (name.empty() || name == "." || name == "..")
    return false;
  for (const char character : name) {
    if (!(character == '-' || character == '_' || character == '.' ||
          (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9')))
      return false;
  }
  return true;
}

std::optional<std::filesystem::path> child_directory(std::string_view base, std::string_view name) {
  if (base.empty() || !valid_application_name(name))
    return std::nullopt;
  const std::filesystem::path root{base};
  if (!root.is_absolute())
    return std::nullopt;
  return root / std::filesystem::path{name};
}

std::string_view environment_value(const char *name) {
  const char *value = std::getenv(name);
  return value ? std::string_view{value} : std::string_view{};
}

} // namespace

std::optional<std::filesystem::path> resolve_user_data_directory(std::string_view application_name,
                                                                 Environment environment) {
#if defined(__ANDROID__)
  (void)application_name;
  (void)environment;
  return std::nullopt;
#elif defined(_WIN32)
  if (const auto directory = child_directory(environment.appdata, application_name))
    return directory;
  return child_directory(environment.home, application_name);
#elif defined(__APPLE__)
  if (const auto home = child_directory(environment.home, "Library"))
    return child_directory(home->string() + "/Application Support", application_name);
  return std::nullopt;
#else
  if (const auto directory = child_directory(environment.xdg_config_home, application_name))
    return directory;
  if (const auto config = child_directory(environment.home, ".config"))
    return child_directory(config->string(), application_name);
  return std::nullopt;
#endif
}

std::optional<std::filesystem::path> user_data_directory(std::string_view application_name) {
  {
    const std::lock_guard lock{override_mutex};
    if (!app_private_directory.empty())
      return app_private_directory;
  }
  return resolve_user_data_directory(application_name, {environment_value("HOME"),
                                                        environment_value("XDG_CONFIG_HOME"),
                                                        environment_value("APPDATA")});
}

bool set_user_data_directory(std::filesystem::path directory) {
  if (!directory.empty() && !directory.is_absolute())
    return false;
  const std::lock_guard lock{override_mutex};
  app_private_directory = std::move(directory);
  return true;
}

bool ensure_user_data_directory(const std::filesystem::path &directory, std::string &error) {
  if (directory.empty() || !directory.is_absolute()) {
    error = "directory must be an absolute path";
    return false;
  }
  std::error_code status;
  if (std::filesystem::exists(directory, status)) {
    if (status || !std::filesystem::is_directory(directory, status)) {
      error = status ? status.message() : "path exists but is not a directory";
      return false;
    }
  } else if (!std::filesystem::create_directories(directory, status) && status) {
    error = status.message();
    return false;
  }
#ifndef _WIN32
  if (::chmod(directory.c_str(), 0700) != 0) {
    error = "could not apply private directory permissions";
    return false;
  }
#endif
  return true;
}

} // namespace lucent::platform

extern "C" const char *lucent_platform_user_data_directory(const char *application_name) {
  static thread_local std::string result;
  if (!application_name)
    return nullptr;
  const auto directory = lucent::platform::user_data_directory(application_name);
  result = directory ? directory->string() : std::string{};
  return result.empty() ? nullptr : result.c_str();
}

extern "C" int lucent_platform_set_user_data_directory(const char *directory) {
  return lucent::platform::set_user_data_directory(directory ? directory : "") ? 1 : 0;
}

extern "C" int lucent_platform_ensure_user_data_directory(const char *application_name) {
  const auto directory =
      application_name ? lucent::platform::user_data_directory(application_name) : std::nullopt;
  if (!directory)
    return 0;
  std::string error;
  return lucent::platform::ensure_user_data_directory(*directory, error) ? 1 : 0;
}

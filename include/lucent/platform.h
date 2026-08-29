#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace lucent::platform {

struct Environment {
  std::string_view home;
  std::string_view xdg_config_home;
  std::string_view appdata;
};

// Resolve the conventional per-application user-data directory for the supplied environment.
// Android intentionally has no environment fallback: its Activity must provide app-private data.
std::optional<std::filesystem::path> resolve_user_data_directory(std::string_view application_name,
                                                                 Environment environment);

// Resolve using the current host environment, or the app-private root supplied by an Android
// Activity through set_user_data_directory(). The application name is one path component.
std::optional<std::filesystem::path> user_data_directory(std::string_view application_name);

// Set an exact app-private directory supplied by a platform shell. An empty path clears it.
// Non-empty paths must be absolute and are rejected without changing the current override.
bool set_user_data_directory(std::filesystem::path directory);

// Create the directory and parents, then apply private owner-only permissions where supported.
bool ensure_user_data_directory(const std::filesystem::path &directory, std::string &error);

} // namespace lucent::platform

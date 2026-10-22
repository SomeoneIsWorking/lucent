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
  std::string_view xdg_data_home;
  std::string_view xdg_cache_home;
  std::string_view xdg_runtime_dir;
  // Windows' LOCALAPPDATA.
  std::string_view local_appdata;
  // macOS' per-user TMPDIR.
  std::string_view tmpdir;
};

// Resolve the conventional per-application user-data directory for the supplied environment.
// Android intentionally has no environment fallback: its Activity must provide app-private data.
std::optional<std::filesystem::path> resolve_user_data_directory(std::string_view application_name,
                                                                 Environment environment);

// The application's own data: XDG_DATA_HOME or $HOME/.local/share on Linux, as the user-data
// directory on macOS and Windows.
std::optional<std::filesystem::path> resolve_data_directory(std::string_view application_name,
                                                            Environment environment);

// The application's cache: XDG_CACHE_HOME or $HOME/.cache on Linux, $HOME/Library/Caches on macOS,
// LOCALAPPDATA/<name>/cache on Windows.
std::optional<std::filesystem::path> resolve_cache_directory(std::string_view application_name,
                                                             Environment environment);

// Per-login files such as sockets: XDG_RUNTIME_DIR or /run/user/<uid> on Linux, TMPDIR on macOS,
// LOCALAPPDATA/Temp on Windows.
std::optional<std::filesystem::path> resolve_runtime_directory(std::string_view application_name,
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

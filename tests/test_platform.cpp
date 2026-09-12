#include "lucent/platform.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

int main() {
  using lucent::platform::Environment;
  auto root = std::filesystem::temp_directory_path() / "lucent-platform-fixture";
  auto home = (root / "home").string();
  auto config = (root / "config").string();
  auto appdata = (root / "appdata").string();
  auto resolved =
      lucent::platform::resolve_user_data_directory("xmen2", Environment{home, config, appdata});
#if defined(_WIN32)
  assert(resolved && *resolved == root / "appdata" / "xmen2");
#elif defined(__APPLE__)
  assert(resolved && *resolved == root / "home" / "Library" / "Application Support" / "xmen2");
#else
  assert(resolved && *resolved == root / "config" / "xmen2");
  auto fallback =
      lucent::platform::resolve_user_data_directory("xmen2", Environment{home, "", appdata});
  assert(fallback && *fallback == root / "home" / ".config" / "xmen2");
#endif

  auto invalid_name = lucent::platform::resolve_user_data_directory(
      "../escape", Environment{home, config, appdata});
  assert(!invalid_name);

  auto relative_root =
      lucent::platform::resolve_user_data_directory("port", {"player", "config", ""});
  assert(!relative_root);

  lucent::platform::set_user_data_directory(root / "private");
  auto override_path = lucent::platform::user_data_directory("any-app");
  assert(override_path && *override_path == root / "private");
  assert(!lucent::platform::set_user_data_directory("relative"));
  assert(lucent::platform::set_user_data_directory({}));

  auto temporary = std::filesystem::current_path() / "lucent-platform-test";
  std::string error;
  assert(lucent::platform::ensure_user_data_directory(temporary, error));
  assert(std::filesystem::is_directory(temporary));
  assert(!lucent::platform::ensure_user_data_directory("relative", error));
  std::filesystem::remove_all(temporary);
  std::cout << "platform: path resolution, validation, override, and creation passed\n";
}

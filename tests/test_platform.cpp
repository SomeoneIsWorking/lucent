#include "lucent/platform.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

int main() {
  using lucent::platform::Environment;
  const auto linux_path = lucent::platform::resolve_user_data_directory(
      "xmen2", {"/home/player", "/home/player/.config", ""});
  assert(linux_path && *linux_path == std::filesystem::path{"/home/player/.config/xmen2"});

  const auto xdg_path =
      lucent::platform::resolve_user_data_directory("port", {"/home/player", "/mnt/config", ""});
  assert(xdg_path && *xdg_path == std::filesystem::path{"/mnt/config/port"});

  const auto invalid_name = lucent::platform::resolve_user_data_directory(
      "../escape", Environment{"/home/player", "/home/player/.config", ""});
  assert(!invalid_name);

  const auto relative_root =
      lucent::platform::resolve_user_data_directory("port", {"player", "config", ""});
  assert(!relative_root);

  lucent::platform::set_user_data_directory("/data/user/0/example/files");
  const auto override_path = lucent::platform::user_data_directory("any-app");
  assert(override_path && *override_path == std::filesystem::path{"/data/user/0/example/files"});
  assert(!lucent::platform::set_user_data_directory("relative"));
  assert(lucent::platform::set_user_data_directory({}));

  const auto temporary = std::filesystem::current_path() / "lucent-platform-test";
  std::string error;
  assert(lucent::platform::ensure_user_data_directory(temporary, error));
  assert(std::filesystem::is_directory(temporary));
  assert(!lucent::platform::ensure_user_data_directory("relative", error));
  std::filesystem::remove_all(temporary);
  std::cout << "platform: path resolution, validation, override, and creation passed\n";
}

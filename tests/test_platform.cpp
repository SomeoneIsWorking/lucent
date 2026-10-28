#include "lucent/platform.h"
#include "test_environment.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

int main() {
  return lucent::test::run_main([] {
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

    const std::string data_root = (root / "data").string();
    const std::string cache_root = (root / "cache").string();
    const std::string run_root = (root / "run").string();
    const std::string local_root = (root / "local").string();
    const std::string tmp_root = (root / "tmp").string();
    const Environment full{.home = home,
                           .xdg_config_home = config,
                           .appdata = appdata,
                           .xdg_data_home = data_root,
                           .xdg_cache_home = cache_root,
                           .xdg_runtime_dir = run_root,
                           .local_appdata = local_root,
                           .tmpdir = tmp_root};
    const Environment bare{.home = home};
    auto data = lucent::platform::resolve_data_directory("xmen2", full);
    auto cache = lucent::platform::resolve_cache_directory("xmen2", full);
    auto runtime = lucent::platform::resolve_runtime_directory("xmen2", full);
#if defined(_WIN32)
    assert(data && *data == root / "appdata" / "xmen2");
    assert(cache && *cache == root / "local" / "xmen2" / "cache");
    assert(runtime && *runtime == root / "local" / "Temp" / "xmen2");
#elif defined(__APPLE__)
    assert(data && *data == root / "home" / "Library" / "Application Support" / "xmen2");
    assert(cache && *cache == root / "home" / "Library" / "Caches" / "xmen2");
    assert(runtime && *runtime == root / "tmp" / "xmen2");
#else
    assert(data && *data == root / "data" / "xmen2");
    assert(cache && *cache == root / "cache" / "xmen2");
    assert(runtime && *runtime == root / "run" / "xmen2");
    auto data_fallback = lucent::platform::resolve_data_directory("xmen2", bare);
    assert(data_fallback && *data_fallback == root / "home" / ".local" / "share" / "xmen2");
    auto cache_fallback = lucent::platform::resolve_cache_directory("xmen2", bare);
    assert(cache_fallback && *cache_fallback == root / "home" / ".cache" / "xmen2");
    auto runtime_fallback = lucent::platform::resolve_runtime_directory("xmen2", bare);
    assert(runtime_fallback && runtime_fallback->parent_path().parent_path() == "/run/user");
#endif
    assert(!lucent::platform::resolve_cache_directory("../escape", full));

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

    auto executable = lucent::platform::executable_path();
    assert(executable && executable->is_absolute() &&
           std::filesystem::is_regular_file(*executable));
    std::cout << "platform: path resolution (config, data, cache, runtime), validation, override, "
                 "creation and executable path passed\n";
    return 0;
  });
}

#include "lucent/web.h"

#include <cerrno>
#include <cstring>

#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

extern "C" int lucent_web_mount_storage(const char *mountpoint) {
  if (!mountpoint || mountpoint[0] != '/' || !mountpoint[1] || std::strchr(mountpoint + 1, '/') ||
      emscripten_is_main_browser_thread()) {
    errno = EINVAL;
    return 0;
  }
  const backend_t storage = wasmfs_create_opfs_backend();
  return wasmfs_create_directory(mountpoint, 0700, storage) == 0;
}

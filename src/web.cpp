#include "lucent/web.h"

#include <cerrno>
#include <cstring>

#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

static bool valid_mountpoint(const char *mountpoint) {
  if (!mountpoint || mountpoint[0] != '/' || !mountpoint[1] || std::strchr(mountpoint + 1, '/') ||
      emscripten_is_main_browser_thread()) {
    errno = EINVAL;
    return false;
  }
  return true;
}

static int mount_result(int result) {
  if (result < 0)
    errno = -result;
  return result == 0;
}

extern "C" int lucent_web_mount_storage(const char *mountpoint) {
  if (!valid_mountpoint(mountpoint))
    return 0;
  const backend_t storage = wasmfs_create_opfs_backend();
  return mount_result(wasmfs_create_directory(mountpoint, 0700, storage));
}

extern "C" int lucent_web_unmount_storage(const char *mountpoint) {
  if (!valid_mountpoint(mountpoint))
    return 0;
  return mount_result(wasmfs_unmount(mountpoint));
}

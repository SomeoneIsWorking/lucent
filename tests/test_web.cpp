#include "lucent/web.h"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

int main() {
  assert(!emscripten_is_main_browser_thread());
  assert(!lucent_web_mount_storage(nullptr));
  assert(errno == EINVAL);
  assert(!lucent_web_mount_storage("/"));
  assert(!lucent_web_mount_storage("relative"));
  assert(!lucent_web_mount_storage("/nested/mount"));
  assert(lucent_web_mount_storage("/opfs"));
  const char *path = "/opfs/lucent-storage-test";
  FILE *file = std::fopen(path, "wb");
  assert(file);
  const char message[] = "OPFS worker read/write";
  assert(std::fwrite(message, 1, sizeof(message), file) == sizeof(message));
  assert(std::fclose(file) == 0);
  file = std::fopen(path, "rb");
  assert(file);
  char result[sizeof(message)]{};
  assert(std::fread(result, 1, sizeof(result), file) == sizeof(result));
  assert(std::fclose(file) == 0);
  assert(std::memcmp(message, result, sizeof(message)) == 0);
  assert(std::remove(path) == 0);
  std::puts("lucent web storage: worker mount/read/write/remove passed; 4 invalid paths refused");
  wasmfs_flush();
  return 0;
}

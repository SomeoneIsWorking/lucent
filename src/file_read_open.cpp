#include "file_read_open.h"

#include <gio/gunixinputstream.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>

namespace lucent::file_read::detail {

GInputStream *open_regular_file(const char *path, std::size_t limit, GError **error,
                                const OpenFunction &open_file) {
  const int descriptor = open_file(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (descriptor < 0) {
    const int reason = errno;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(reason),
                "Could not open selected file: %s", g_strerror(reason));
    return nullptr;
  }
  struct stat information{};
  if (fstat(descriptor, &information) != 0) {
    const int reason = errno;
    close(descriptor);
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(reason),
                "Could not inspect opened file: %s", g_strerror(reason));
    return nullptr;
  }
  if (!S_ISREG(information.st_mode)) {
    close(descriptor);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE,
                        "Selected path is not a regular file");
    return nullptr;
  }
  if (information.st_size < 0 || static_cast<std::uintmax_t>(information.st_size) > limit) {
    close(descriptor);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Selected file exceeds the byte limit");
    return nullptr;
  }
  return g_unix_input_stream_new(descriptor, true);
}

} // namespace lucent::file_read::detail

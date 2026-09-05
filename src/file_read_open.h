#pragma once

#include <gio/gio.h>

#include <cstddef>
#include <functional>

namespace lucent::file_read::detail {

using OpenFunction = std::function<int(const char *, int)>;

// Run in an I/O worker. Validation belongs to the opened descriptor, never prior path metadata.
// The injectable syscall boundary lets tests replace the pathname at the instant of open.
GInputStream *open_regular_file(const char *path, std::size_t limit, GError **error,
                                const OpenFunction &open_file);

} // namespace lucent::file_read::detail

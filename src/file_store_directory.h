#pragma once

#include <lucent/file_store.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string_view>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace lucent::file_store::detail {

using WriteFunction = std::function<ssize_t(int, const void *, std::size_t)>;
using RemoveFunction = std::function<int(int, const char *, int)>;

// Staging belongs exclusively to I/O workers. request_close()/closed() are nonblocking
// lifetime signals; terminal cleanup serializes with an in-flight writer before returning.
class Directory {
public:
  explicit Directory(std::filesystem::path parent);
  Result stage(std::string_view bytes, std::string_view suffix, const WriteFunction &write_bytes,
               const RemoveFunction &remove_file = ::unlinkat);
  void request_close() noexcept;
  [[nodiscard]] bool closed() const noexcept;
  std::string cleanup();

private:
  std::string prepare();
  std::filesystem::path parent_;
  std::string child_;
  int parent_descriptor_{-1};
  int directory_descriptor_{-1};
  std::vector<std::string> files_;
  std::string cleanup_failure_;
  std::atomic<bool> closed_{};
  std::mutex mutex_;
};

} // namespace lucent::file_store::detail

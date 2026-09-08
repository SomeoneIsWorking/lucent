#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace lucent::file_store {

struct Result {
  std::filesystem::path path;
  std::string error;
};

// Main-thread asynchronous immutable backing files. The supplied existing parent must be
// absolute, owned by the current user, and not group/other writable.
// Trailing separators are removed so they cannot bypass final-component symlink refusal.
// Construction does no I/O. Each store lazily owns a distinct private child directory.
class FileStore {
public:
  FileStore(std::filesystem::path trusted_parent, std::size_t max_total_bytes,
            std::size_t max_files);
  ~FileStore();
  FileStore(const FileStore &) = delete;
  FileStore &operator=(const FileStore &) = delete;
  FileStore(FileStore &&) = delete;
  FileStore &operator=(FileStore &&) = delete;

  // Owns the input bytes until completion. Only suffixes such as ".mp3" are accepted
  // (dot followed by 1-16 ASCII letters/digits). Busy/closed/invalid input throws.
  // Budget refusal is a deferred error result and preserves every previously published file.
  void start(std::string bytes, std::string suffix);
  // One nonblocking GLib iteration; returns a completed result exactly once.
  std::optional<Result> poll();
  [[nodiscard]] bool active() const noexcept;
  // Terminal teardown: suppresses pending results, drains a writer, and removes owned files
  // before returning. This is the only blocking operation; close after consumers release files.
  void close() noexcept;

private:
  struct State;
  std::shared_ptr<State> state_;
};

} // namespace lucent::file_store

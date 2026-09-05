#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace lucent::file_read {

struct Result {
  std::string bytes;
  std::string error;
};

// Optional Linux/GIO bounded asynchronous local-file reader. Use on the application's main
// thread. An error never exposes partial bytes. File content interpretation belongs to consumers.
class FileRead {
public:
  FileRead();
  ~FileRead();
  FileRead(const FileRead &) = delete;
  FileRead &operator=(const FileRead &) = delete;
  FileRead(FileRead &&) = delete;
  FileRead &operator=(FileRead &&) = delete;

  // Throws logic_error while active and invalid_argument for invalid paths/limits.
  // A zero-byte limit permits only an empty file; the limit must allow a max+1 size probe.
  void start(const std::filesystem::path &path, std::size_t max_bytes);
  // Dispatches one nonblocking GLib context iteration (also while idle), then returns a
  // completed result exactly once. Continue host polling to reap detached cancelled operations.
  std::optional<Result> poll();
  // Cancels and detaches pending work; late GIO callbacks cannot access this owner.
  void close() noexcept;
  [[nodiscard]] bool active() const noexcept;

private:
  struct State;
  std::shared_ptr<State> state_;
};

} // namespace lucent::file_read

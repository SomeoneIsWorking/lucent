#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace lucent::file_dialog {

enum class Status { Selected, Cancelled, Error };

struct Result {
  Status status{Status::Cancelled};
  std::filesystem::path path;
  std::string error;
};

// Optional Linux/GTK3 single-file picker. Construct, call, and destroy on the application's
// main thread. It never reads the selected file or starts a blocking GTK event loop.
class FileDialog {
public:
  using Completion = std::function<void(Result)>;

  FileDialog();
  ~FileDialog();
  FileDialog(const FileDialog &) = delete;
  FileDialog &operator=(const FileDialog &) = delete;
  FileDialog(FileDialog &&) = delete;
  FileDialog &operator=(FileDialog &&) = delete;

  // Throws invalid_argument for an empty callback or embedded NUL title, and logic_error
  // if a request is already active. Runtime failures are delivered as Status::Error.
  void open(std::string_view title, Completion completion);
  // Dispatches one nonblocking GTK context iteration and then any completed request.
  // Completion runs only here, after dialog teardown; it may reopen or destroy this owner.
  void poll();
  // Requests a Cancelled completion on the next poll. No-op while idle.
  void cancel();
  // Suppresses completion, tears down the active dialog, and returns to idle.
  void close() noexcept;
  [[nodiscard]] bool active() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lucent::file_dialog

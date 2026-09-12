#include <lucent/file_store.h>
#include <lucent/log.h>

#include "file_store_directory.h"

#include <gio/gio.h>
#include <unistd.h>

#include <stdexcept>
#include <string_view>
#include <utility>

namespace lucent::file_store {

struct FileStore::State {
  std::shared_ptr<detail::Directory> directory;
  std::size_t byte_limit{};
  std::size_t file_limit{};
  std::size_t reserved_bytes{};
  std::size_t reserved_files{};
  bool pending{};
  std::optional<Result> result;

  struct Request {
    std::shared_ptr<State> state;
    std::string bytes;
    std::string suffix;
  };

  static void stage_worker(GTask *task, gpointer, gpointer task_data, GCancellable *) {
    const auto &request = *static_cast<Request *>(task_data);
    auto result = request.state->directory->stage(
        request.bytes, request.suffix, [](int descriptor, const void *bytes, std::size_t count) {
          return ::write(descriptor, bytes, count);
        });
    g_task_return_pointer(
        task, new Result(std::move(result)), +[](gpointer data) {
          delete static_cast<Result *>(data);
        });
  }

  static void staged(GObject *, GAsyncResult *completion, gpointer) {
    GTask *task = G_TASK(completion);
    const auto &request = *static_cast<Request *>(g_task_get_task_data(task));
    std::unique_ptr<Result> result(static_cast<Result *>(g_task_propagate_pointer(task, nullptr)));
    if (request.state->directory->closed()) {
      return;
    }
    if (!result->error.empty()) {
      request.state->reserved_bytes -= request.bytes.size();
      --request.state->reserved_files;
    }
    request.state->result = std::move(*result);
  }
};

FileStore::FileStore(std::filesystem::path trusted_parent, std::size_t max_total_bytes,
                     std::size_t max_files) {
  while (!trusted_parent.has_filename() && trusted_parent != trusted_parent.root_path()) {
    trusted_parent = trusted_parent.parent_path();
  }
  if (!trusted_parent.is_absolute() || trusted_parent == trusted_parent.root_path() ||
      trusted_parent.native().find('\0') != std::string::npos || max_files == 0) {
    throw std::invalid_argument(
        "File store requires a non-root absolute trusted parent and a positive file budget");
  }
  state_ = std::make_shared<State>();
  state_->directory = std::make_shared<detail::Directory>(std::move(trusted_parent));
  state_->byte_limit = max_total_bytes;
  state_->file_limit = max_files;
}

FileStore::~FileStore() {
  close();
}

void FileStore::start(std::string bytes, std::string suffix) {
  if (state_ == nullptr) {
    throw std::logic_error("File store is closed");
  }
  if (active()) {
    throw std::logic_error("A file-store request is already active");
  }
  if (suffix.size() < 2 || suffix.size() > 17 || suffix.front() != '.' ||
      suffix.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
                               1) != std::string::npos) {
    throw std::invalid_argument("File-store suffix must be a dot and 1-16 ASCII letters/digits");
  }
  if (bytes.size() > state_->byte_limit - state_->reserved_bytes ||
      state_->reserved_files == state_->file_limit) {
    state_->result = Result{{}, "File-store count or byte budget exceeded"};
    state_->pending = true;
    return;
  }
  auto request =
      std::make_unique<State::Request>(State::Request{state_, std::move(bytes), std::move(suffix)});
  state_->reserved_bytes += request->bytes.size();
  ++state_->reserved_files;
  state_->pending = true;
  GTask *task = g_task_new(nullptr, nullptr, State::staged, nullptr);
  g_task_set_task_data(
      task, request.release(), +[](gpointer data) {
        delete static_cast<State::Request *>(data);
      });
  g_task_run_in_thread(task, State::stage_worker);
  g_object_unref(task);
}

std::optional<Result> FileStore::poll() {
  g_main_context_iteration(nullptr, false);
  if (state_ == nullptr || !state_->result.has_value()) {
    return std::nullopt;
  }
  auto result = std::move(state_->result);
  state_->result.reset();
  state_->pending = false;
  return result;
}

bool FileStore::active() const noexcept {
  return state_ != nullptr && state_->pending;
}

void FileStore::close() noexcept {
  if (state_ == nullptr) {
    return;
  }
  state_->directory->request_close();
  // This terminal boundary runs after consumers release their decoders/file handles. It
  // drains a writer already holding the directory lock; a queued writer observes closed.
  const auto error = state_->directory->cleanup();
  if (!error.empty()) {
    lucent::log(lucent::Level::Error, "file-store", error);
  }
  state_.reset();
}

} // namespace lucent::file_store

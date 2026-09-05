#include <lucent/file_read.h>

#include "file_read_open.h"

#include <gio/gio.h>

#include <fcntl.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace lucent::file_read {

struct FileRead::State : std::enable_shared_from_this<State> {
  GCancellable *cancellable{g_cancellable_new()};
  GInputStream *stream{};
  std::string path;
  std::size_t limit{};
  std::string bytes;
  std::optional<Result> result;
  bool detached{};

  ~State() {
    g_clear_object(&stream);
    g_clear_object(&cancellable);
  }

  // Each GIO operation retains only this state, never FileRead or a consumer callback.
  gpointer retain() { return new std::shared_ptr<State>(shared_from_this()); }
  static std::shared_ptr<State> take(gpointer data) {
    std::unique_ptr<std::shared_ptr<State>> retained(static_cast<std::shared_ptr<State> *>(data));
    return std::move(*retained);
  }

  void fail(std::string message) {
    bytes.clear();
    if (!detached) {
      result = Result{{}, std::move(message)};
    }
  }

  bool failed(GError *error) {
    if (error != nullptr) {
      fail(error->message);
      g_error_free(error);
      return true;
    }
    return detached;
  }

  static void open_worker(GTask *task, gpointer, gpointer task_data, GCancellable *) {
    const auto &state = *static_cast<std::shared_ptr<State> *>(task_data);
    if (g_task_return_error_if_cancelled(task)) {
      return;
    }
    GError *error = nullptr;
    GInputStream *stream =
        detail::open_regular_file(state->path.c_str(), state->limit, &error,
                                  [](const char *path, int flags) { return ::open(path, flags); });
    if (error != nullptr) {
      g_task_return_error(task, error);
    } else {
      g_task_return_pointer(task, stream, g_object_unref);
    }
  }

  static void opened(GObject *, GAsyncResult *completion, gpointer data) {
    auto state = take(data);
    GError *error = nullptr;
    state->stream =
        static_cast<GInputStream *>(g_task_propagate_pointer(G_TASK(completion), &error));
    if (!state->failed(error)) {
      state->read_next();
    }
  }

  void read_next() {
    // Probe one byte past the limit even when the file grows after its metadata was queried.
    const auto count = std::min<std::size_t>(64 * 1024, limit - bytes.size() + 1);
    g_input_stream_read_bytes_async(G_INPUT_STREAM(stream), count, G_PRIORITY_DEFAULT, cancellable,
                                    read_ready, retain());
  }

  static void read_ready(GObject *source, GAsyncResult *completion, gpointer data) {
    auto state = take(data);
    GError *error = nullptr;
    GBytes *chunk = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), completion, &error);
    if (state->failed(error)) {
      if (chunk != nullptr) {
        g_bytes_unref(chunk);
      }
      return;
    }
    gsize count = 0;
    const auto *content = static_cast<const char *>(g_bytes_get_data(chunk, &count));
    if (count > state->limit - state->bytes.size()) {
      g_bytes_unref(chunk);
      state->fail("Selected file exceeds the byte limit");
      return;
    }
    if (count != 0) {
      state->bytes.append(content, count);
    }
    g_bytes_unref(chunk);
    if (count == 0) {
      state->result = Result{std::move(state->bytes), {}};
    } else {
      state->read_next();
    }
  }
};

FileRead::FileRead() = default;
FileRead::~FileRead() {
  close();
}

void FileRead::start(const std::filesystem::path &path, std::size_t max_bytes) {
  if (active()) {
    throw std::logic_error("A file read is already active");
  }
  const auto &native_path = path.native();
  if (native_path.empty() || native_path.find('\0') != std::string::npos ||
      max_bytes >= std::string{}.max_size() ||
      max_bytes == std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument("File read requires a NUL-free path and a bounded byte limit");
  }
  state_ = std::make_shared<State>();
  state_->limit = max_bytes;
  state_->path = native_path;
  GTask *task = g_task_new(nullptr, state_->cancellable, State::opened, state_->retain());
  g_task_set_task_data(
      task, state_->retain(),
      +[](gpointer data) { delete static_cast<std::shared_ptr<State> *>(data); });
  g_task_run_in_thread(task, State::open_worker);
  g_object_unref(task);
}

std::optional<Result> FileRead::poll() {
  g_main_context_iteration(nullptr, false);
  if (state_ != nullptr && state_->result.has_value()) {
    auto result = std::move(state_->result);
    state_.reset();
    return result;
  }
  return std::nullopt;
}

void FileRead::close() noexcept {
  if (state_ != nullptr) {
    state_->detached = true;
    g_cancellable_cancel(state_->cancellable);
    state_.reset();
  }
}

bool FileRead::active() const noexcept {
  return state_ != nullptr;
}

} // namespace lucent::file_read

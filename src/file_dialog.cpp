#include <lucent/file_dialog.h>

#include <gtk/gtk.h>

#include <optional>
#include <stdexcept>
#include <utility>

namespace lucent::file_dialog {

struct FileDialog::Impl {
  GtkFileChooserNative *dialog{};
  gulong response_handler{};
  Completion completion;
  std::optional<Result> result;

  ~Impl() { close(); }

  void release_dialog() noexcept {
    if (dialog != nullptr) {
      g_signal_handler_disconnect(dialog, response_handler);
      gtk_native_dialog_destroy(GTK_NATIVE_DIALOG(dialog));
      g_object_unref(dialog);
      dialog = nullptr;
      response_handler = 0;
      // The embedding renderer does not run GTK's main loop. Submit native hide/destroy
      // requests now, before completion can cause it to stop polling this idle owner.
      gdk_display_flush(gdk_display_get_default());
    }
  }

  void close() noexcept {
    release_dialog();
    completion = {};
    result.reset();
  }

  void response(int response_id) {
    if (result.has_value()) {
      return;
    }
    if (response_id == GTK_RESPONSE_CANCEL || response_id == GTK_RESPONSE_DELETE_EVENT) {
      result = Result{};
    } else if (response_id != GTK_RESPONSE_ACCEPT) {
      result = Result{Status::Error, {}, "File chooser returned an unexpected response"};
    } else {
      GSList *filenames = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dialog));
      if (filenames == nullptr || filenames->next != nullptr || filenames->data == nullptr) {
        result = Result{Status::Error, {}, "File chooser did not return exactly one local file"};
      } else {
        result = Result{Status::Selected, static_cast<const char *>(filenames->data), {}};
      }
      g_slist_free_full(filenames, g_free);
    }
  }
};

FileDialog::FileDialog() : impl_(std::make_unique<Impl>()) {
}
FileDialog::~FileDialog() = default;

void FileDialog::open(std::string_view title, Completion completion) {
  if (active()) {
    throw std::logic_error("A file dialog request is already active");
  }
  if (!completion || title.find('\0') != std::string_view::npos) {
    throw std::invalid_argument("File dialog requires a callback and a NUL-free title");
  }
  impl_->completion = std::move(completion);
  if (!gtk_init_check(nullptr, nullptr)) {
    impl_->result = Result{Status::Error, {}, "GTK could not connect to the desktop display"};
    return;
  }
  impl_->dialog = gtk_file_chooser_native_new(std::string(title).c_str(), nullptr,
                                              GTK_FILE_CHOOSER_ACTION_OPEN, "Open", "Cancel");
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(impl_->dialog), false);
  gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(impl_->dialog), true);
  impl_->response_handler =
      g_signal_connect(impl_->dialog, "response",
                       G_CALLBACK(+[](GtkNativeDialog *, gint response_id, gpointer data) {
                         static_cast<Impl *>(data)->response(response_id);
                       }),
                       impl_.get());
  gtk_native_dialog_show(GTK_NATIVE_DIALOG(impl_->dialog));
}

void FileDialog::poll() {
  if (!active()) {
    return;
  }
  if (!impl_->result.has_value()) {
    g_main_context_iteration(nullptr, false);
  }
  if (impl_->result.has_value()) {
    auto completion = std::move(impl_->completion);
    auto result = std::move(impl_->result.value());
    impl_->close();
    completion(std::move(result));
  }
}

void FileDialog::cancel() {
  if (active() && !impl_->result.has_value()) {
    impl_->result = Result{};
    impl_->release_dialog();
  }
}

void FileDialog::close() noexcept {
  impl_->close();
}
bool FileDialog::active() const noexcept {
  return static_cast<bool>(impl_->completion);
}

} // namespace lucent::file_dialog

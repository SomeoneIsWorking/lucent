#include <lucent/file_dialog.h>
#include <lucent/log.h>

#include <gtk/gtk.h>

#include <gdk/gdkx.h>
#include <X11/Xlib.h>

// Xlib's unscoped macro would replace the public file-dialog enum name below.
#undef Status

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using lucent::file_dialog::FileDialog;
using lucent::file_dialog::Result;
using lucent::file_dialog::Status;

void require(bool value, const char *message) {
  if (!value) {
    throw std::runtime_error(message);
  }
}

template <typename Predicate> void until(FileDialog &picker, Predicate ready) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!ready()) {
    require(std::chrono::steady_clock::now() < deadline, "GTK did not reach the expected state");
    picker.poll();
    std::this_thread::yield();
  }
}

GtkWidget *chooser_window() {
  GList *windows = gtk_window_list_toplevels();
  GtkWidget *found = nullptr;
  for (GList *item = windows; item != nullptr; item = item->next) {
    auto *widget = GTK_WIDGET(item->data);
    if (GTK_IS_FILE_CHOOSER_DIALOG(widget) && gtk_widget_get_visible(widget)) {
      require(found == nullptr, "More than one visible GTK file chooser");
      found = widget;
    }
  }
  g_list_free(windows);
  return found;
}

void require_unmapped(Window window) {
  Display *observer = XOpenDisplay(nullptr);
  require(observer != nullptr, "Could not open independent X11 observation connection");
  static int observed_error = 0;
  observed_error = 0;
  const auto previous_handler = XSetErrorHandler(+[](Display *, XErrorEvent *error) {
    observed_error = error->error_code;
    return 0;
  });
  XWindowAttributes attributes{};
  const int found = XGetWindowAttributes(observer, window, &attributes);
  XSync(observer, false);
  XSetErrorHandler(previous_handler);
  XCloseDisplay(observer);
  require(observed_error == 0 || observed_error == BadWindow,
          "Unexpected X11 error while observing chooser teardown");
  require(found == 0 || attributes.map_state != IsViewable,
          "Completion left chooser visible to an independent X11 connection");
}

void lifecycle_and_selection(const std::filesystem::path &file) {
  FileDialog picker;
  std::vector<Result> results;
  auto completed = [&](Result result) {
    require(!picker.active(), "Callback ran before request teardown");
    results.push_back(std::move(result));
  };
  require(!picker.active(), "New picker should be idle");
  picker.poll();
  picker.cancel();
  bool rejected = false;
  try {
    picker.open("invalid", {});
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected && !picker.active(), "Empty completion must be rejected without activating");
  rejected = false;
  try {
    picker.open(std::string_view("bad\0title", 9), completed);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected && !picker.active(), "Embedded NUL title must be rejected");

  picker.open("Lucent file selection", completed);
  require(picker.active() && results.empty(), "Open must not complete synchronously");
  rejected = false;
  try {
    picker.open("must not replace selection", completed);
  } catch (const std::logic_error &) {
    rejected = true;
  }
  require(rejected && picker.active(), "Busy request must preserve the active dialog");
  until(picker, [] { return chooser_window() != nullptr; });
  auto *chooser = GTK_FILE_CHOOSER(chooser_window());
  const Window chooser_xid = gdk_x11_window_get_xid(gtk_widget_get_window(GTK_WIDGET(chooser)));
  require(!gtk_file_chooser_get_select_multiple(chooser), "Picker must be single-file");
  require(gtk_file_chooser_get_local_only(chooser), "Picker must return local files");
  require(gtk_file_chooser_set_filename(chooser, file.c_str()), "Failed to choose fixture file");
  until(picker, [&] {
    char *selected = gtk_file_chooser_get_filename(chooser);
    const bool matches = selected != nullptr && std::filesystem::path(selected) == file;
    g_free(selected);
    return matches;
  });
  gtk_dialog_response(GTK_DIALOG(chooser), GTK_RESPONSE_ACCEPT);
  require(results.empty(), "GTK response must defer the application callback until poll");
  until(picker, [&] { return !results.empty(); });
  require_unmapped(chooser_xid);
  require(results.size() == 1 && results[0].status == Status::Selected && results[0].path == file &&
              results[0].error.empty(),
          "Real GTK selection did not return exactly the fixture path");
  picker.poll();
  require(results.size() == 1, "Selection callback must not repeat");

  picker.open("Lucent cancellation", completed);
  until(picker, [] { return chooser_window() != nullptr; });
  const Window cancelled_xid = gdk_x11_window_get_xid(gtk_widget_get_window(chooser_window()));
  gtk_dialog_response(GTK_DIALOG(chooser_window()), GTK_RESPONSE_CANCEL);
  until(picker, [&] { return results.size() == 2; });
  require_unmapped(cancelled_xid);
  require(results.back().status == Status::Cancelled && results.back().path.empty(),
          "Real GTK cancellation must not select a file");

  picker.open("Lucent programmatic cancellation", completed);
  picker.cancel();
  require(results.size() == 2, "Programmatic cancellation must also defer completion");
  picker.poll();
  require(results.size() == 3 && results.back().status == Status::Cancelled,
          "Programmatic cancellation must complete exactly once");
  picker.open("Lucent close suppression", completed);
  until(picker, [] { return chooser_window() != nullptr; });
  const Window closed_xid = gdk_x11_window_get_xid(gtk_widget_get_window(chooser_window()));
  picker.close();
  require_unmapped(closed_xid);
  picker.poll();
  require(results.size() == 3 && !picker.active(), "Close must suppress completion");

  picker.open("Lucent reentrant completion",
              [&](const Result &) { picker.open("reopened", completed); });
  picker.cancel();
  picker.poll();
  require(picker.active(), "Completion must be allowed to reopen the picker");
  picker.close();

  auto owned = std::make_unique<FileDialog>();
  owned->open("Destroy from completion", [&](const Result &) { owned.reset(); });
  owned->cancel();
  owned->poll();
  require(owned == nullptr, "Completion must be allowed to destroy its picker");
  {
    FileDialog abandoned;
    abandoned.open("Destroy pending request", completed);
  }
  for (int iteration = 0; iteration < 20; ++iteration) {
    g_main_context_iteration(nullptr, false);
  }
  require(results.size() == 3, "Destroyed request must not deliver a late callback");
}

void missing_display() {
  FileDialog picker;
  std::vector<Result> results;
  picker.open("No display", [&](Result result) { results.push_back(std::move(result)); });
  require(results.empty() && picker.active(), "Initialization failure must be deferred");
  picker.poll();
  require(results.size() == 1 && results[0].status == Status::Error && results[0].path.empty() &&
              !results[0].error.empty() && !picker.active(),
          "Missing display must report an explicit error and return to idle");
}
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 2, "Expected fixture path or --no-display");
    if (std::string_view(argv[1]) == "--no-display") {
      missing_display();
    } else {
      lifecycle_and_selection(std::filesystem::absolute(argv[1]));
    }
    lucent::log(lucent::Level::Info, "file-dialog-test", "All file-dialog checks passed");
    return 0;
  } catch (const std::exception &error) {
    lucent::log(lucent::Level::Error, "file-dialog-test", error.what());
    return 1;
  }
}

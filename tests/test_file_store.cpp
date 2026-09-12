#include <lucent/file_store.h>
#include <lucent/log.h>

#include "../src/file_store_directory.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using lucent::file_store::FileStore;
using lucent::file_store::Result;

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::string read(const std::filesystem::path &file) {
  std::ifstream input(file, std::ios::binary);
  require(static_cast<bool>(input), "Could not open staged file");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

Result finish(FileStore &store) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto result = store.poll()) {
      require(!store.active(), "Completion must clear the pending request");
      require(!store.poll().has_value(), "Result must be delivered once");
      return std::move(result.value());
    }
    std::this_thread::yield();
  }
  throw std::runtime_error("File-store request did not complete");
}

mode_t mode(const std::filesystem::path &path) {
  struct stat information = {};
  require(stat(path.c_str(), &information) == 0, "Could not inspect file permissions");
  return information.st_mode & 0777;
}

void binary_and_isolation(const std::filesystem::path &parent) {
  FileStore store(parent, 16, 4);
  const std::string binary("\0\xff\xc0\x80\0hello", 10);
  require(std::filesystem::is_empty(parent), "Construction must not create files");
  store.start(binary, ".mp3");
  require(store.active(), "Staging must be asynchronous");
  bool rejected = false;
  try {
    store.start("must not replace pending", ".mp3");
  } catch (const std::logic_error &) {
    rejected = true;
  }
  require(rejected, "Concurrent stage must be refused");
  auto first = finish(store);
  require(first.error.empty() && read(first.path) == binary,
          "All binary bytes must survive staging");
  require(first.path.extension() == ".mp3" && mode(first.path) == 0400 &&
              mode(first.path.parent_path()) == 0700 && mode(parent) == 0755,
          "Owned child/files must be private without chmod of the caller parent");
  store.start("next", ".mp3");
  auto second = finish(store);
  require(second.error.empty() && second.path != first.path && read(first.path) == binary &&
              read(second.path) == "next",
          "A new backing file must never overwrite a previous path");
  FileStore other(parent, 32, 1);
  other.start("independent", ".bin");
  auto independent = finish(other);
  require(independent.error.empty() && independent.path.parent_path() != first.path.parent_path(),
          "Different stores must own distinct children");
  store.close();
  require(!std::filesystem::exists(first.path.parent_path()) &&
              read(independent.path) == "independent",
          "Close must immediately clean only its own files without another GLib poll");
  require(!store.poll().has_value(), "Closed store must not publish late results");
  rejected = false;
  try {
    store.start("closed", ".bin");
  } catch (const std::logic_error &) {
    rejected = true;
  }
  require(rejected, "Close is terminal");
  other.close();
  require(std::filesystem::is_empty(parent), "Both stores must release their directories");
}

void budgets_and_errors(const std::filesystem::path &parent) {
  {
    FileStore store(parent, 2, 4);
    store.start("ab", ".dat");
    const auto valid = finish(store);
    store.start("c", ".dat");
    const auto refused = finish(store);
    require(!refused.error.empty() && refused.path.empty() && read(valid.path) == "ab",
            "Byte-budget refusal must preserve earlier files");
    store.start({}, ".dat");
    require(finish(store).error.empty(), "Budget refusal must not consume a file reservation");
  }
  {
    FileStore store(parent, 32, 1);
    store.start("first", ".dat");
    const auto valid = finish(store);
    store.start({}, ".dat");
    require(!finish(store).error.empty() && read(valid.path) == "first",
            "File count counts empty files too and preserves previous paths");
    for (const auto *suffix :
         {"", ".", "../bad", ".a/b", ".a\\b", ".audio.file", ".12345678901234567"}) {
      bool rejected = false;
      try {
        store.start({}, suffix);
      } catch (const std::invalid_argument &) {
        rejected = true;
      }
      require(rejected && !store.active(), "Invalid suffix must not activate a request");
    }
  }
  const auto absent = parent / "created-later";
  FileStore store(absent, 4, 1);
  require(!std::filesystem::exists(absent), "Constructor must not touch a missing parent");
  store.start("data", ".bin");
  require(!finish(store).error.empty(), "Missing parent must report a deferred error");
  std::filesystem::create_directory(absent);
  require(chmod(absent.c_str(), 0777) == 0, "Could not seed unsafe parent");
  store.start("data", ".bin");
  require(!finish(store).error.empty(), "Group/world-writable parent must be refused");
  require(chmod(absent.c_str(), 0755) == 0, "Could not restore trusted parent");
  store.start("data", ".bin");
  require(finish(store).error.empty(), "Failed staging must release byte/count reservations");
  store.close();
  require(std::filesystem::remove(absent), "Recovered parent must be empty after close");
  const auto target = parent / "symlink-target";
  const auto link = parent / "symlink-parent";
  std::filesystem::create_directory(target);
  std::filesystem::create_directory_symlink(target, link);
  FileStore linked(std::filesystem::path(link.string() + "/"), 4, 1);
  linked.start("data", ".bin");
  require(!finish(linked).error.empty() && std::filesystem::is_empty(target),
          "Trailing slash must not bypass the symlink-parent refusal");
  linked.close();
  require(std::filesystem::remove(link) && std::filesystem::remove(target),
          "Symlink discriminator must not leave staged files");
}

void worker_failures_and_cancel(const std::filesystem::path &parent) {
  lucent::file_store::detail::Directory directory(parent);
  const auto failure =
      directory.stage("data", ".bin", [](int, const void *, std::size_t) -> ssize_t {
        errno = ENOSPC;
        return -1;
      });
  require(!failure.error.empty() && failure.path.empty(), "Failed write must not publish a path");
  const auto valid = directory.stage("retained", ".bin", ::write);
  require(valid.error.empty() && read(valid.path) == "retained",
          "Failure must not prevent another stage");
  const auto cancelled = directory.stage(std::string(65537, 'x'), ".bin",
                                         [&](int descriptor, const void *data, std::size_t count) {
                                           const auto written = ::write(descriptor, data, count);
                                           directory.request_close();
                                           return written;
                                         });
  require(!cancelled.error.empty() && cancelled.path.empty() && read(valid.path) == "retained",
          "Cancellation during a real partial write must not publish or damage prior files");
  require(directory.cleanup().empty() && std::filesystem::is_empty(parent),
          "Cancelled and successful backing files must both be removed during teardown");
}

void failed_removal_freezes_writes(const std::filesystem::path &parent) {
  lucent::file_store::detail::Directory directory(parent);
  const auto valid = directory.stage("retained", ".bin", ::write);
  require(valid.error.empty(), "Could not seed retained backing file");
  bool wrote_partial = false;
  const auto failure = directory.stage(
      "partial", ".bin",
      [&](int descriptor, const void *bytes, std::size_t) -> ssize_t {
        if (!wrote_partial) {
          wrote_partial = true;
          return ::write(descriptor, bytes, 1);
        }
        errno = ENOSPC;
        return -1;
      },
      [](int, const char *, int) {
        errno = EACCES;
        return -1;
      });
  const auto count_files = [&] {
    return std::distance(std::filesystem::directory_iterator(valid.path.parent_path()),
                         std::filesystem::directory_iterator{});
  };
  const auto after_failure = count_files();
  const auto refused = directory.stage("must not reach disk", ".bin", ::write);
  const bool frozen = !refused.error.empty() && refused.path.empty() &&
                      count_files() == after_failure && read(valid.path) == "retained";
  directory.request_close();
  const auto cleanup_error = directory.cleanup();
  require(!failure.error.empty() && failure.path.empty() && after_failure == 2,
          "Removal discriminator must leave one unpublished partial file beside the valid file");
  require(cleanup_error.empty() && std::filesystem::is_empty(parent),
          "Terminal cleanup must retry removing the failed output and retain no owned files");
  require(frozen, "Failed removal must freeze future writes without damaging published files");
}

void immediate_exit(const std::filesystem::path &executable, const std::filesystem::path &parent) {
  const pid_t child = fork();
  require(child >= 0, "Could not fork teardown discriminator");
  if (child == 0) {
    execl(executable.c_str(), executable.c_str(), "--exit-child", parent.c_str(), nullptr);
    _exit(127);
  }
  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  require(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "Fresh-process teardown child failed");
  require(std::filesystem::is_empty(parent),
          "Normal process exit must leave neither completed nor pending backing files");
}

void child_stage(const std::filesystem::path &parent) {
  FileStore store(parent, 16 * 1024 * 1024, 2);
  store.start("completed before shutdown", ".bin");
  require(finish(store).error.empty(), "Child must publish one backing file before teardown");
  store.start(std::string(8 * 1024 * 1024, 'x'), ".bin");
  // No more GLib pumping: destructor must close/drain/clean before the process returns.
}
} // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 3 && std::string_view(argv[1]) == "--exit-child") {
      child_stage(std::filesystem::path(argv[2]));
      return 0;
    }
    require(argc == 2, "Expected a scoped fixture parent");
    const auto parent = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(parent);
    require(std::filesystem::is_empty(parent), "File-store fixture parent must be empty");
    require(chmod(parent.c_str(), 0755) == 0, "Could not set fixture parent mode");
    binary_and_isolation(parent);
    budgets_and_errors(parent);
    worker_failures_and_cancel(parent);
    failed_removal_freezes_writes(parent);
    immediate_exit(std::filesystem::absolute(argv[0]), parent);
    require(std::filesystem::remove(parent), "Fixture parent must be empty after all teardowns");
    lucent::log(lucent::Level::Info, "file-store-test",
                "All immutable asynchronous file-store checks passed");
    return 0;
  } catch (const std::exception &error) {
    lucent::log(lucent::Level::Error, "file-store-test", error.what());
    return 1;
  }
}

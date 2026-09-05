#include <lucent/file_read.h>
#include <lucent/log.h>

#include "../src/file_read_open.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using lucent::file_read::FileRead;
using lucent::file_read::Result;

void require(bool value, const char *message) {
  if (!value) {
    throw std::runtime_error(message);
  }
}

Result finish(FileRead &reader) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto result = reader.poll()) {
      require(!reader.active(), "Completed read must become idle before delivery");
      require(!reader.poll().has_value(), "Completed read must not deliver twice");
      return std::move(result.value());
    }
    std::this_thread::yield();
  }
  throw std::runtime_error("Async file read did not complete within five seconds");
}

void write(const std::filesystem::path &path, const std::string &bytes) {
  std::ofstream output(path, std::ios::binary);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();
  require(static_cast<bool>(output), "Could not write the synthetic fixture");
}

void replacement_at_open(const std::filesystem::path &path) {
  write(path, "regular");
  require(std::filesystem::is_regular_file(path), "Replacement discriminator must begin regular");
  GError *error = nullptr;
  int opened = -1;
  auto *stream = lucent::file_read::detail::open_regular_file(
      path.c_str(), 128, &error, [&](const char *selected, int flags) {
        require((flags & O_NONBLOCK) != 0 && (flags & O_CLOEXEC) != 0,
                "Production open must prevent FIFO blocking and descriptor inheritance");
        require(std::filesystem::remove(path), "Could not replace regular fixture");
        require(mkfifo(path.c_str(), 0600) == 0, "Could not swap a FIFO at the open boundary");
        opened = ::open(selected, flags);
        return opened;
      });
  require(stream == nullptr && error != nullptr && error->code == G_IO_ERROR_NOT_REGULAR_FILE,
          "Replacement FIFO must be rejected from the actual opened descriptor");
  g_error_free(error);
  require(opened >= 0 && fcntl(opened, F_GETFD) == -1 && errno == EBADF,
          "Rejected FIFO descriptor must be closed");
  require(std::filesystem::remove(path), "Could not remove FIFO replacement");

  write(path, "original");
  error = nullptr;
  stream = lucent::file_read::detail::open_regular_file(
      path.c_str(), 128, &error, [&](const char *selected, int flags) {
        opened = ::open(selected, flags);
        require(opened >= 0, "Could not open original fixture");
        require(std::filesystem::remove(path), "Could not unlink opened fixture");
        write(path, "replacement");
        return opened;
      });
  require(stream != nullptr && error == nullptr, "Opened original regular file must be accepted");
  char buffer[32]{};
  const auto count = g_input_stream_read(stream, buffer, sizeof(buffer), nullptr, &error);
  require(count == 8 && std::string_view(buffer, 8) == "original" && error == nullptr,
          "Reader must retain the validated descriptor instead of reopening its replaced path");
  g_object_unref(stream);
  require(fcntl(opened, F_GETFD) == -1 && errno == EBADF,
          "Accepted stream must own descriptor cleanup");
  require(std::filesystem::remove(path), "Could not remove replacement fixture");
}

void tests(const std::filesystem::path &directory) {
  std::filesystem::create_directories(directory);
  const auto payload_file = directory / "payload";
  const auto empty_file = directory / "empty";
  const auto fifo_file = directory / "fifo";
  replacement_at_open(directory / "replacement");
  std::string payload(131072, 'x');
  payload[0] = '\0';
  payload[65536] = static_cast<char>(255);
  write(payload_file, payload);
  write(empty_file, {});
  require(!std::filesystem::exists(fifo_file), "FIFO fixture path must be fresh");
  require(mkfifo(fifo_file.c_str(), 0600) == 0, "Could not create FIFO negative fixture");

  FileRead reader;
  require(!reader.active() && !reader.poll().has_value(), "New reader must be idle");
  reader.close();
  reader.start(payload_file, payload.size());
  bool rejected = false;
  try {
    reader.start(empty_file, 0);
  } catch (const std::logic_error &) {
    rejected = true;
  }
  require(rejected && reader.active(), "Busy reader must preserve its active operation");
  auto result = finish(reader);
  require(result.error.empty() && result.bytes == payload,
          "Asynchronous chunks must preserve all bytes at the exact limit");

  reader.start(empty_file, 0);
  result = finish(reader);
  require(result.error.empty() && result.bytes.empty(), "Zero limit must accept an empty file");
  for (const auto &path : {payload_file, directory, fifo_file, directory / "absent"}) {
    reader.start(path, payload.size() - 1);
    result = finish(reader);
    require(!result.error.empty() && result.bytes.empty(),
            "Oversize/nonregular/missing input must fail without partial content");
  }
  // procfs reports zero metadata size for this regular file. Its content exercises the
  // runtime max+1 check independently of the metadata-size refusal above.
  const std::filesystem::path dynamic_file("/proc/self/stat");
  require(std::filesystem::is_regular_file(dynamic_file) &&
              std::filesystem::file_size(dynamic_file) == 0,
          "Expected zero-size regular procfs discriminator is unavailable");
  reader.start(dynamic_file, 0);
  result = finish(reader);
  require(result.error == "Selected file exceeds the byte limit" && result.bytes.empty(),
          "Read-time byte limit must reject content beyond reported metadata size");

  for (const auto &invalid :
       {std::filesystem::path{}, std::filesystem::path(std::string("a\0b", 3))}) {
    rejected = false;
    try {
      reader.start(invalid, 10);
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    require(rejected && !reader.active(), "Invalid path must not activate a read");
  }
  rejected = false;
  try {
    reader.start(payload_file, std::numeric_limits<std::size_t>::max());
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected && !reader.active(), "Unbounded limit must be rejected");

  reader.start(payload_file, payload.size());
  reader.close();
  require(!reader.active(), "Close must detach immediately");
  reader.start(empty_file, 0);
  result = finish(reader);
  require(result.error.empty() && result.bytes.empty(),
          "Old cancelled operation must not contaminate a new read");
  {
    FileRead destroyed;
    destroyed.start(payload_file, payload.size());
  }
  // A subsequent completed read pumps the same context while cancellation callbacks release
  // their shared states; none retain their destroyed FileRead owner or publish into reader.
  reader.start(payload_file, payload.size());
  result = finish(reader);
  require(result.error.empty() && result.bytes == payload,
          "Destruction with an in-flight read must preserve subsequent operations");

  std::filesystem::remove(payload_file);
  std::filesystem::remove(empty_file);
  std::filesystem::remove(fifo_file);
  std::filesystem::remove(directory);
}
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 2, "Expected scoped fixture directory");
    tests(std::filesystem::path(argv[1]));
    lucent::log(lucent::Level::Info, "file-read-test",
                "All bounded asynchronous file-read checks passed");
    return 0;
  } catch (const std::exception &error) {
    lucent::log(lucent::Level::Error, "file-read-test", error.what());
    return 1;
  }
}

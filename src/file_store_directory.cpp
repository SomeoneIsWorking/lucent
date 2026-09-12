#include "file_store_directory.h"

#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <utility>

namespace lucent::file_store::detail {
namespace {
class Descriptor {
public:
  explicit Descriptor(int value) : value_(value) {
  }
  ~Descriptor() {
    close();
  }
  Descriptor(const Descriptor &) = delete;
  Descriptor &operator=(const Descriptor &) = delete;
  [[nodiscard]] int get() const noexcept {
    return value_;
  }
  int close() noexcept {
    const int value = std::exchange(value_, -1);
    return value >= 0 ? ::close(value) : 0;
  }

private:
  int value_;
};

std::string system_error(std::string_view operation) {
  const int reason = errno;
  return std::string(operation) + ": " + std::strerror(reason);
}

std::string random_name(std::string &error) {
  std::array<unsigned char, 16> random{};
  std::size_t filled = 0;
  while (filled != random.size()) {
    const auto count = getrandom(random.data() + filled, random.size() - filled, 0);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      error = count < 0 ? system_error("Secure file-name generation failed")
                        : "Secure file-name generation made no progress";
      return {};
    }
    filled += static_cast<std::size_t>(count);
  }
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(random.size() * 2);
  for (const auto value : random) {
    result.push_back(hex[value >> 4]);
    result.push_back(hex[value & 15]);
  }
  return result;
}
} // namespace

Directory::Directory(std::filesystem::path parent) : parent_(std::move(parent)) {
}

std::string Directory::prepare() {
  if (directory_descriptor_ >= 0) {
    return {};
  }
  if (parent_descriptor_ >= 0 && !child_.empty()) {
    directory_descriptor_ =
        openat(parent_descriptor_, child_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    return directory_descriptor_ < 0 ? system_error("Cannot open owned backing-file directory")
                                     : "";
  }
  parent_descriptor_ = ::open(parent_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (parent_descriptor_ < 0) {
    return system_error("Cannot open private backing-file parent");
  }
  struct stat information = {};
  std::string error;
  if (fstat(parent_descriptor_, &information) != 0) {
    error = system_error("Cannot inspect backing-file parent");
  } else if (information.st_uid != geteuid() || (information.st_mode & 0022) != 0) {
    error = "Backing-file parent must be owned by the current user and not group/other writable";
  }
  std::string child;
  if (error.empty()) {
    child = "lucent-files-" + random_name(error);
  }
  if (error.empty() && mkdirat(parent_descriptor_, child.c_str(), 0700) != 0) {
    error = system_error("Cannot create unique private backing-file directory");
  }
  if (error.empty()) {
    child_ = std::move(child);
    directory_descriptor_ =
        openat(parent_descriptor_, child_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory_descriptor_ < 0) {
      error = system_error("Cannot open owned backing-file directory");
    }
  }
  if (!error.empty() && child_.empty()) {
    ::close(parent_descriptor_);
    parent_descriptor_ = -1;
  }
  return error;
}

Result Directory::stage(std::string_view bytes, std::string_view suffix,
                        const WriteFunction &write_bytes, const RemoveFunction &remove_file) {
  std::lock_guard lock(mutex_);
  if (closed()) {
    return {{}, "Backing-file store is closed"};
  }
  if (!cleanup_failure_.empty()) {
    return {{}, "Backing-file store cannot stage after cleanup failure: " + cleanup_failure_};
  }
  std::string error = prepare();
  if (!error.empty()) {
    return {{}, std::move(error)};
  }
  std::string name = random_name(error);
  if (!error.empty()) {
    return {{}, std::move(error)};
  }
  name += suffix;
  auto path = parent_ / child_ / name;
  files_.reserve(files_.size() + 1);
  files_.push_back(name);
  Descriptor descriptor(openat(directory_descriptor_, name.c_str(),
                               O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
  if (descriptor.get() < 0) {
    files_.pop_back();
    return {{}, system_error("Cannot create unique backing file")};
  }
  std::size_t written = 0;
  while (written < bytes.size() && !closed()) {
    const auto requested = std::min<std::size_t>(64 * 1024, bytes.size() - written);
    const auto count = write_bytes(descriptor.get(), bytes.data() + written, requested);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      error = count < 0 ? system_error("Backing-file write failed")
                        : "Backing-file write made no progress";
      break;
    }
    if (static_cast<std::size_t>(count) > requested) {
      error = "Backing-file writer exceeded its requested byte count";
      break;
    }
    written += static_cast<std::size_t>(count);
  }
  if (closed()) {
    error = "Backing-file store closed during staging";
  }
  if (error.empty() && fchmod(descriptor.get(), 0400) != 0) {
    error = system_error("Cannot make completed backing file read-only");
  }
  if (descriptor.close() != 0 && error.empty()) {
    error = system_error("Cannot close completed backing file");
  }
  if (!error.empty()) {
    if (remove_file(directory_descriptor_, name.c_str(), 0) != 0) {
      // Reservations may roll back on error, but this output still occupies disk space.
      // Refuse subsequent staging so unremoved failures cannot bypass the storage budget.
      cleanup_failure_ = system_error("Cannot remove failed backing file");
      error += "; " + cleanup_failure_;
    } else {
      files_.pop_back();
    }
    return {{}, std::move(error)};
  }
  return {std::move(path), {}};
}

void Directory::request_close() noexcept {
  closed_.store(true);
}
bool Directory::closed() const noexcept {
  return closed_.load();
}

std::string Directory::cleanup() {
  std::lock_guard lock(mutex_);
  std::string errors;
  for (const auto &file : files_) {
    if (unlinkat(directory_descriptor_, file.c_str(), 0) != 0 && errno != ENOENT) {
      errors += system_error("Cannot remove owned backing file") + "; ";
    }
  }
  files_.clear();
  if (directory_descriptor_ >= 0) {
    ::close(directory_descriptor_);
    directory_descriptor_ = -1;
  }
  if (!child_.empty()) {
    if (unlinkat(parent_descriptor_, child_.c_str(), AT_REMOVEDIR) != 0 && errno != ENOENT) {
      errors += system_error("Cannot remove owned backing-file directory") + "; ";
    }
    child_.clear();
  }
  if (parent_descriptor_ >= 0) {
    ::close(parent_descriptor_);
    parent_descriptor_ = -1;
  }
  return errors;
}

} // namespace lucent::file_store::detail

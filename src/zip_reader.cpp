#include "zip_reader.h"

#include <algorithm>

namespace lucent::zip::detail {
bool ArchiveReader::contains(std::uint64_t offset, std::uint64_t length) const {
  return offset <= size() && length <= size() - offset;
}

bool ArchiveReader::read(std::uint64_t offset, std::span<std::uint8_t> output, std::string &error) {
  if (!contains(offset, output.size())) {
    error = "archive read exceeds its declared size";
    return false;
  }
  while (!output.empty()) {
    const auto chunk = output.first(std::min(output.size(), read_chunk_bytes));
    if (!read_chunk(offset, chunk, error))
      return false;
    offset += chunk.size();
    output = output.subspan(chunk.size());
  }
  return true;
}

bool FileArchive::open(const std::filesystem::path &path, const ExtractionLimits &limits,
                       std::string &error) {
  if (file_.is_open())
    file_.close();
  file_.clear();
  size_ = 0;
  file_.open(path, std::ios::binary | std::ios::ate);
  if (!file_) {
    error = "could not open archive: " + path.string();
    return false;
  }
  const std::streampos end = file_.tellg();
  if (end <= std::streampos{0}) {
    error = "could not inspect archive size: " + path.string();
    file_.close();
    return false;
  }
  const auto size = static_cast<std::uint64_t>(end);
  if (size > limits.max_archive_bytes) {
    error = "archive exceeds the compressed byte limit";
    file_.close();
    return false;
  }
  size_ = size;
  return true;
}

bool FileArchive::read_chunk(std::uint64_t offset, std::span<std::uint8_t> output,
                             std::string &error) {
  file_.seekg(static_cast<std::streamoff>(offset));
  file_.read(reinterpret_cast<char *>(output.data()), static_cast<std::streamsize>(output.size()));
  if (!file_) {
    error = "archive read failed or backing file was truncated at byte " + std::to_string(offset);
    return false;
  }
  return true;
}

bool SpanArchive::read_chunk(std::uint64_t offset, std::span<std::uint8_t> output,
                             std::string &error) {
  (void)error;
  std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset), output.size(), output.begin());
  return true;
}
} // namespace lucent::zip::detail

#pragma once

#include "lucent/zip.h"

#include <fstream>

namespace lucent::zip::detail {
using ByteView = std::span<const std::uint8_t>;
using Bytes = std::vector<std::uint8_t>;
inline constexpr std::size_t read_chunk_bytes = std::size_t{64} * 1024;

// ZIP offsets address the backing archive, never a mapping of it in host memory.
// The boundary checks ranges and caps each physical read, including metadata.
class ArchiveReader {
public:
  virtual ~ArchiveReader() = default;
  virtual std::uint64_t size() const = 0;
  bool contains(std::uint64_t offset, std::uint64_t length) const;
  bool read(std::uint64_t offset, std::span<std::uint8_t> output, std::string &error);

private:
  virtual bool read_chunk(std::uint64_t offset, std::span<std::uint8_t> output,
                          std::string &error) = 0;
};

class FileArchive final : public ArchiveReader {
public:
  bool open(const std::filesystem::path &path, const ExtractionLimits &limits, std::string &error);
  std::uint64_t size() const override {
    return size_;
  }

private:
  bool read_chunk(std::uint64_t offset, std::span<std::uint8_t> output,
                  std::string &error) override;
  std::ifstream file_;
  std::uint64_t size_ = 0;
};

class SpanArchive final : public ArchiveReader {
public:
  explicit SpanArchive(ByteView bytes) : bytes_(bytes) {
  }
  std::uint64_t size() const override {
    return bytes_.size();
  }

private:
  bool read_chunk(std::uint64_t offset, std::span<std::uint8_t> output,
                  std::string &error) override;
  ByteView bytes_;
};
} // namespace lucent::zip::detail

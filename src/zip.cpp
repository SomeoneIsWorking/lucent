#include "lucent/zip.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <vector>

#include <zlib.h>

namespace lucent::zip {
namespace {

using Bytes = std::vector<std::uint8_t>;

std::uint16_t u16(const Bytes &bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

std::uint32_t u32(const Bytes &bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset] | (bytes[offset + 1] << 8) |
                                    (bytes[offset + 2] << 16) | (bytes[offset + 3] << 24));
}

bool has(Bytes const &bytes, std::size_t offset, std::size_t length) {
  return offset <= bytes.size() && length <= bytes.size() - offset;
}

bool equal_name(std::string_view left, std::string_view right) {
  if (left.size() != right.size())
    return false;
  return std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
  });
}

bool safe_name(std::string_view name) {
  if (name.empty() || name.front() == '/' || name.find('\\') != std::string_view::npos)
    return false;
  std::size_t start = 0;
  while (start < name.size()) {
    const std::size_t end = name.find('/', start);
    const std::string_view part =
        name.substr(start, end == std::string_view::npos ? name.size() - start : end - start);
    if (part.empty() || part == "." || part == "..")
      return false;
    if (end == std::string_view::npos)
      break;
    start = end + 1;
  }
  return name.back() != '/';
}

struct Entry {
  std::string name;
  std::uint16_t flags = 0;
  std::uint16_t method = 0;
  std::uint32_t compressed_size = 0;
  std::uint32_t uncompressed_size = 0;
  std::uint32_t local_offset = 0;
};

bool read_file(const std::filesystem::path &path, Bytes &bytes, std::string &error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "could not open archive";
    return false;
  }
  bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  if (input.bad()) {
    error = "could not read archive";
    return false;
  }
  return true;
}

bool entries(const Bytes &bytes, std::vector<Entry> &out, std::string &error) {
  constexpr std::uint32_t end_signature = 0x06054b50;
  constexpr std::uint32_t entry_signature = 0x02014b50;
  const std::size_t lower = bytes.size() > 22 + 0xffff ? bytes.size() - 22 - 0xffff : 0;
  std::size_t end = std::string::npos;
  for (std::size_t offset = bytes.size(); offset-- > lower;) {
    if (has(bytes, offset, 4) && u32(bytes, offset) == end_signature) {
      end = offset;
      break;
    }
  }
  if (end == std::string::npos || !has(bytes, end, 22)) {
    error = "archive has no valid end record";
    return false;
  }
  if (u16(bytes, end + 4) != 0 || u16(bytes, end + 6) != 0 || u32(bytes, end + 16) > bytes.size()) {
    error = "multi-disk archives are not supported";
    return false;
  }
  const std::size_t count = u16(bytes, end + 10);
  const std::size_t central_offset = u32(bytes, end + 16);
  const std::size_t central_size = u32(bytes, end + 12);
  if (u16(bytes, end + 8) != u16(bytes, end + 10) || !has(bytes, central_offset, central_size)) {
    error = "archive central directory is truncated";
    return false;
  }
  if (count == 0xffff) {
    error = "ZIP64 archives are not supported";
    return false;
  }
  std::size_t offset = central_offset;
  for (std::size_t index = 0; index < count; ++index) {
    if (!has(bytes, offset, 46) || u32(bytes, offset) != entry_signature) {
      error = "archive central directory entry is invalid";
      return false;
    }
    const std::size_t name_size = u16(bytes, offset + 28);
    const std::size_t extra_size = u16(bytes, offset + 30);
    const std::size_t comment_size = u16(bytes, offset + 32);
    const std::size_t record_size = 46 + name_size + extra_size + comment_size;
    if (!has(bytes, offset, record_size)) {
      error = "archive central directory entry is truncated";
      return false;
    }
    Entry entry;
    entry.name.assign(reinterpret_cast<const char *>(&bytes[offset + 46]), name_size);
    entry.flags = u16(bytes, offset + 8);
    entry.method = u16(bytes, offset + 10);
    entry.compressed_size = u32(bytes, offset + 20);
    entry.uncompressed_size = u32(bytes, offset + 24);
    entry.local_offset = u32(bytes, offset + 42);
    const bool directory = !entry.name.empty() && entry.name.back() == '/';
    const std::string_view checked_name =
        directory ? std::string_view(entry.name).substr(0, entry.name.size() - 1)
                  : std::string_view(entry.name);
    if (!safe_name(checked_name)) {
      error = "archive contains an unsafe path";
      return false;
    }
    out.push_back(std::move(entry));
    offset += record_size;
  }
  return true;
}

bool inflate_raw(const std::uint8_t *source, std::size_t source_size, std::uint8_t *destination,
                 std::size_t destination_size) {
  z_stream stream{};
  stream.next_in = const_cast<Bytef *>(source);
  stream.avail_in = static_cast<uInt>(source_size);
  stream.next_out = destination;
  stream.avail_out = static_cast<uInt>(destination_size);
  if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
    return false;
  const int result = inflate(&stream, Z_FINISH);
  const bool complete = result == Z_STREAM_END && stream.total_in == source_size &&
                        stream.total_out == destination_size;
  inflateEnd(&stream);
  return complete;
}

bool extract_entry(const Bytes &bytes, const Entry &entry, const std::filesystem::path &destination,
                   std::filesystem::path &path, std::string &error) {
  if ((entry.flags & 1) != 0 || entry.method != 0 && entry.method != 8) {
    error = "archive entry is encrypted or uses an unsupported compression method";
    return false;
  }
  if (!has(bytes, entry.local_offset, 30) || u32(bytes, entry.local_offset) != 0x04034b50) {
    error = "archive local entry is invalid";
    return false;
  }
  const std::size_t name_size = u16(bytes, entry.local_offset + 26);
  const std::size_t extra_size = u16(bytes, entry.local_offset + 28);
  const std::size_t data_offset = entry.local_offset + 30 + name_size + extra_size;
  if (!has(bytes, data_offset, entry.compressed_size)) {
    error = "archive entry data is truncated";
    return false;
  }
  if (entry.method == 0 && entry.compressed_size != entry.uncompressed_size) {
    error = "stored archive entry has inconsistent sizes";
    return false;
  }
  path = destination / std::filesystem::path(entry.name);
  std::error_code filesystem_error;
  std::filesystem::create_directories(path.parent_path(), filesystem_error);
  if (filesystem_error) {
    error = "could not create extracted archive directory";
    return false;
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    error = "could not create extracted archive file";
    return false;
  }
  if (entry.method == 0) {
    output.write(reinterpret_cast<const char *>(&bytes[data_offset]), entry.uncompressed_size);
  } else {
    std::vector<std::uint8_t> unpacked(entry.uncompressed_size);
    if (!inflate_raw(&bytes[data_offset], entry.compressed_size, unpacked.data(),
                     unpacked.size())) {
      error = "archive entry failed deflate decompression";
      return false;
    }
    output.write(reinterpret_cast<const char *>(unpacked.data()), unpacked.size());
  }
  if (!output) {
    error = "could not write extracted archive file";
    return false;
  }
  return true;
}

} // namespace

bool extract_install(const std::filesystem::path &archive, const std::filesystem::path &destination,
                     std::string_view required_name, std::filesystem::path &executable,
                     std::string &error) {
  Bytes bytes;
  std::vector<Entry> archive_entries;
  if (!read_file(archive, bytes, error) || !entries(bytes, archive_entries, error))
    return false;
  const auto matches = std::count_if(
      archive_entries.begin(), archive_entries.end(), [required_name](const Entry &entry) {
        return entry.name.back() != '/' &&
               equal_name(std::filesystem::path(entry.name).filename().string(), required_name);
      });
  if (matches != 1) {
    error = matches == 0 ? "archive does not contain the required executable"
                         : "archive contains more than one matching executable";
    return false;
  }
  std::error_code filesystem_error;
  std::filesystem::create_directories(destination, filesystem_error);
  if (filesystem_error) {
    error = "could not create extraction directory";
    return false;
  }
  for (const Entry &entry : archive_entries) {
    if (entry.name.back() == '/') {
      std::filesystem::create_directories(destination / std::filesystem::path(entry.name),
                                          filesystem_error);
      if (filesystem_error) {
        error = "could not create extracted archive directory";
        return false;
      }
      continue;
    }
    std::filesystem::path extracted;
    if (!extract_entry(bytes, entry, destination, extracted, error))
      return false;
    if (equal_name(extracted.filename().string(), required_name))
      executable = extracted;
  }
  return !executable.empty();
}

} // namespace lucent::zip

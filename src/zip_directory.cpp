#include "zip_directory.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <unordered_set>

namespace lucent::zip::detail {
std::uint16_t u16(ByteView bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

std::uint32_t u32(ByteView bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

bool has(ByteView bytes, std::size_t offset, std::size_t length) {
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
  if (name.empty() || name.front() == '/' || name.find('\\') != std::string_view::npos ||
      name.find('\0') != std::string_view::npos)
    return false;
  std::size_t start = 0;
  while (start < name.size()) {
    const std::size_t end = name.find('/', start);
    const std::string_view part =
        name.substr(start, end == std::string_view::npos ? name.size() - start : end - start);
    if (part.empty() || part == "." || part == ".." || part.find(':') != std::string_view::npos ||
        part.back() == '.' || part.back() == ' ')
      return false;
    const std::size_t extension = part.find('.');
    std::string stem{part.substr(0, extension)};
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    const bool numbered_device = stem.size() == 4 &&
                                 (stem.starts_with("com") || stem.starts_with("lpt")) &&
                                 stem[3] >= '1' && stem[3] <= '9';
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" || numbered_device)
      return false;
    if (end == std::string_view::npos)
      break;
    start = end + 1;
  }
  return name.back() != '/';
}

std::string normalized_name(std::string_view name) {
  if (!name.empty() && name.back() == '/')
    name.remove_suffix(1);
  std::string normalized{name};
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
  return normalized;
}

bool reserve_budget(std::uint64_t amount, std::uint64_t limit, std::uint64_t &used,
                    std::string_view error_message, std::string &error) {
  if (used > limit || amount > limit - used) {
    error = error_message;
    return false;
  }
  used += amount;
  return true;
}

bool validate_file_directory_shapes(const std::vector<Entry> &archive_entries, std::string &error) {
  std::unordered_set<std::string> files;
  for (const Entry &entry : archive_entries) {
    if (entry.name.back() != '/')
      files.insert(normalized_name(entry.name));
  }
  for (const std::string &file : files) {
    std::size_t separator = file.find('/');
    while (separator != std::string::npos) {
      if (files.contains(file.substr(0, separator))) {
        error = "archive path is both a file and a parent directory";
        return false;
      }
      separator = file.find('/', separator + 1);
    }
  }
  return true;
}

bool entries(ArchiveReader &archive, std::vector<Entry> &out, const ExtractionLimits &limits,
             Budget &budget, std::string &error) {
  if (!reserve_budget(archive.size(), limits.max_archive_bytes, budget.archive_bytes,
                      "archives exceed the combined compressed byte limit", error))
    return false;

  constexpr std::uint32_t end_signature = 0x06054b50;
  constexpr std::uint32_t entry_signature = 0x02014b50;
  const auto tail_size =
      static_cast<std::size_t>(std::min<std::uint64_t>(archive.size(), 22 + 0xffff));
  const std::uint64_t tail_offset = archive.size() - tail_size;
  Bytes tail(tail_size);
  if (!archive.read(tail_offset, tail, error))
    return false;
  ByteView bytes{tail};
  std::size_t end = std::string::npos;
  for (std::size_t offset = bytes.size(); offset-- > 0;) {
    if (has(bytes, offset, 4) && u32(bytes, offset) == end_signature) {
      end = offset;
      break;
    }
  }
  if (end == std::string::npos || !has(bytes, end, 22)) {
    error = "archive has no valid end record";
    return false;
  }
  if (!has(bytes, end, 22 + u16(bytes, end + 20))) {
    error = "archive end record is truncated";
    return false;
  }
  if (u16(bytes, end + 4) != 0 || u16(bytes, end + 6) != 0 ||
      u32(bytes, end + 16) > archive.size()) {
    error = "multi-disk archives are not supported";
    return false;
  }
  const std::size_t count = u16(bytes, end + 10);
  const std::uint64_t central_offset = u32(bytes, end + 16);
  const std::uint64_t central_size = u32(bytes, end + 12);
  const std::uint64_t end_offset = tail_offset + end;
  if (u16(bytes, end + 8) != u16(bytes, end + 10) || central_offset > end_offset ||
      !archive.contains(central_offset, central_size) ||
      central_size != end_offset - central_offset) {
    error = "archive central directory is truncated";
    return false;
  }
  if (count == 0xffff) {
    error = "ZIP64 archives are not supported";
    return false;
  }
  if (budget.entries > limits.max_entries || count > limits.max_entries - budget.entries) {
    error = "archives exceed the combined entry-count limit";
    return false;
  }
  budget.entries += count;

  std::uint64_t offset = central_offset;
  const std::uint64_t central_end = central_offset + central_size;
  std::unordered_set<std::string> names;
  for (std::size_t index = 0; index < count; ++index) {
    std::array<std::uint8_t, 46> header{};
    if (offset > central_end || header.size() > central_end - offset ||
        !archive.contains(offset, header.size())) {
      error = "archive central directory entry is invalid";
      return false;
    }
    if (!archive.read(offset, header, error))
      return false;
    if (u32(header, 0) != entry_signature) {
      error = "archive central directory entry is invalid";
      return false;
    }
    const std::size_t name_size = u16(header, 28);
    const std::size_t extra_size = u16(header, 30);
    const std::size_t comment_size = u16(header, 32);
    const std::size_t record_size = 46 + name_size + extra_size + comment_size;
    if (!archive.contains(offset, record_size) || record_size > central_end - offset) {
      error = "archive central directory entry is truncated";
      return false;
    }
    Bytes name(name_size);
    if (!archive.read(offset + 46, name, error))
      return false;
    Entry entry;
    entry.name.assign(name.begin(), name.end());
    entry.flags = u16(header, 8);
    entry.method = u16(header, 10);
    entry.crc = u32(header, 16);
    entry.compressed_size = u32(header, 20);
    entry.uncompressed_size = u32(header, 24);
    entry.local_offset = u32(header, 42);
    if (entry.compressed_size == std::numeric_limits<std::uint32_t>::max() ||
        entry.uncompressed_size == std::numeric_limits<std::uint32_t>::max() ||
        entry.local_offset == std::numeric_limits<std::uint32_t>::max()) {
      error = "ZIP64 archive entries are not supported";
      return false;
    }
    if (entry.uncompressed_size > limits.max_entry_bytes) {
      error = "archive entry exceeds the expanded byte limit";
      return false;
    }
    if (!reserve_budget(entry.uncompressed_size, limits.max_extracted_bytes, budget.extracted_bytes,
                        "archive exceeds the total expanded byte limit across archive levels",
                        error))
      return false;
    const bool directory = !entry.name.empty() && entry.name.back() == '/';
    const std::string_view checked_name =
        directory ? std::string_view(entry.name).substr(0, entry.name.size() - 1)
                  : std::string_view(entry.name);
    if (!safe_name(checked_name)) {
      error = "archive contains an unsafe path";
      return false;
    }
    if (!names.insert(normalized_name(entry.name)).second) {
      error = "archive contains duplicate output paths";
      return false;
    }
    out.push_back(std::move(entry));
    offset += record_size;
  }
  if (offset != central_end) {
    error = "archive central directory size does not match its entries";
    return false;
  }
  return validate_file_directory_shapes(out, error);
}

bool local_data_offset(ArchiveReader &archive, const Entry &entry, std::uint64_t &data_offset,
                       std::string &error) {
  std::array<std::uint8_t, 30> header{};
  if (!archive.contains(entry.local_offset, header.size())) {
    error = "archive local entry is invalid";
    return false;
  }
  if (!archive.read(entry.local_offset, header, error))
    return false;
  if (u32(header, 0) != 0x04034b50) {
    error = "archive local entry is invalid";
    return false;
  }
  const std::uint16_t local_flags = u16(header, 6);
  const std::size_t name_size = u16(header, 26);
  const std::size_t extra_size = u16(header, 28);
  const std::uint64_t name_offset = static_cast<std::uint64_t>(entry.local_offset) + 30;
  data_offset = name_offset + name_size + extra_size;
  if (!archive.contains(name_offset, name_size + extra_size)) {
    error = "archive local entry disagrees with its central directory";
    return false;
  }
  Bytes name(name_size);
  if (!archive.read(name_offset, name, error))
    return false;
  const bool descriptor = (entry.flags & 8U) != 0;
  const auto agrees_or_descriptor_zero = [descriptor](std::uint32_t local, std::uint32_t central) {
    return local == central || (descriptor && local == 0);
  };
  if (!std::equal(name.begin(), name.end(), entry.name.begin(), entry.name.end()) ||
      local_flags != entry.flags || u16(header, 8) != entry.method ||
      !agrees_or_descriptor_zero(u32(header, 14), entry.crc) ||
      !agrees_or_descriptor_zero(u32(header, 18), entry.compressed_size) ||
      !agrees_or_descriptor_zero(u32(header, 22), entry.uncompressed_size)) {
    error = "archive local entry disagrees with its central directory";
    return false;
  }
  if (!archive.contains(data_offset, entry.compressed_size)) {
    error = "archive entry data is truncated";
    return false;
  }
  return true;
}
} // namespace lucent::zip::detail

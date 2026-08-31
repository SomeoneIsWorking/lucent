#include "lucent/zip.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <fstream>
#include <limits>
#include <span>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <zlib.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace lucent::zip {
namespace {

using ByteView = std::span<const std::uint8_t>;
using Bytes = std::vector<std::uint8_t>;

struct Budget {
  std::uint64_t archive_bytes = 0;
  std::uint64_t extracted_bytes = 0;
  std::size_t entries = 0;
};

struct Entry {
  std::string name;
  std::uint16_t flags = 0;
  std::uint16_t method = 0;
  std::uint32_t crc = 0;
  std::uint32_t compressed_size = 0;
  std::uint32_t uncompressed_size = 0;
  std::uint32_t local_offset = 0;
};

struct Candidate {
  const Entry *entry = nullptr;
  bool nested = false;
};

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

class MappedArchive {
public:
  MappedArchive() = default;
  MappedArchive(const MappedArchive &) = delete;
  MappedArchive &operator=(const MappedArchive &) = delete;

  ~MappedArchive() { reset(); }

  bool open(const std::filesystem::path &path, const ExtractionLimits &limits, std::string &error) {
    reset();
    std::error_code status;
    const std::uintmax_t size = std::filesystem::file_size(path, status);
    if (status || size == 0) {
      error = "could not inspect archive size: " + path.string();
      return false;
    }
    if (size > limits.max_archive_bytes) {
      error = "archive exceeds the compressed byte limit";
      return false;
    }
    if (size > std::numeric_limits<std::size_t>::max()) {
      error = "archive cannot be addressed on this platform";
      return false;
    }
    size_ = static_cast<std::size_t>(size);
#if defined(_WIN32)
    file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
      error = "could not open archive: " + path.string();
      return false;
    }
    mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping_ == nullptr) {
      error = "could not map archive: " + path.string();
      reset();
      return false;
    }
    data_ = static_cast<const std::uint8_t *>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
    if (data_ == nullptr) {
      error = "could not map archive: " + path.string();
      reset();
      return false;
    }
#else
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
      error = "could not open archive: " + path.string();
      return false;
    }
    void *mapping = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, descriptor, 0);
    (void)close(descriptor);
    if (mapping == MAP_FAILED) {
      error = "could not map archive: " + path.string();
      return false;
    }
    data_ = static_cast<const std::uint8_t *>(mapping);
#endif
    return true;
  }

  ByteView bytes() const { return {data_, size_}; }

private:
  void reset() {
#if defined(_WIN32)
    if (data_ != nullptr)
      UnmapViewOfFile(data_);
    if (mapping_ != nullptr)
      CloseHandle(mapping_);
    if (file_ != INVALID_HANDLE_VALUE)
      CloseHandle(file_);
    mapping_ = nullptr;
    file_ = INVALID_HANDLE_VALUE;
#else
    if (data_ != nullptr)
      munmap(const_cast<std::uint8_t *>(data_), size_);
#endif
    data_ = nullptr;
    size_ = 0;
  }

  const std::uint8_t *data_ = nullptr;
  std::size_t size_ = 0;
#if defined(_WIN32)
  HANDLE file_ = INVALID_HANDLE_VALUE;
  HANDLE mapping_ = nullptr;
#endif
};

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

bool entries(ByteView bytes, std::vector<Entry> &out, const ExtractionLimits &limits,
             Budget &budget, std::string &error) {
  if (!reserve_budget(bytes.size(), limits.max_archive_bytes, budget.archive_bytes,
                      "archives exceed the combined compressed byte limit", error))
    return false;

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
  if (!has(bytes, end, 22 + u16(bytes, end + 20))) {
    error = "archive end record is truncated";
    return false;
  }
  if (u16(bytes, end + 4) != 0 || u16(bytes, end + 6) != 0 || u32(bytes, end + 16) > bytes.size()) {
    error = "multi-disk archives are not supported";
    return false;
  }
  const std::size_t count = u16(bytes, end + 10);
  const std::size_t central_offset = u32(bytes, end + 16);
  const std::size_t central_size = u32(bytes, end + 12);
  if (u16(bytes, end + 8) != u16(bytes, end + 10) || central_offset > end ||
      !has(bytes, central_offset, central_size) || central_size != end - central_offset) {
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

  std::size_t offset = central_offset;
  const std::size_t central_end = central_offset + central_size;
  std::unordered_set<std::string> names;
  for (std::size_t index = 0; index < count; ++index) {
    if (!has(bytes, offset, 46) || u32(bytes, offset) != entry_signature) {
      error = "archive central directory entry is invalid";
      return false;
    }
    const std::size_t name_size = u16(bytes, offset + 28);
    const std::size_t extra_size = u16(bytes, offset + 30);
    const std::size_t comment_size = u16(bytes, offset + 32);
    const std::size_t record_size = 46 + name_size + extra_size + comment_size;
    if (offset > central_end || !has(bytes, offset, record_size) ||
        record_size > central_end - offset) {
      error = "archive central directory entry is truncated";
      return false;
    }
    Entry entry;
    entry.name.assign(reinterpret_cast<const char *>(&bytes[offset + 46]), name_size);
    entry.flags = u16(bytes, offset + 8);
    entry.method = u16(bytes, offset + 10);
    entry.crc = u32(bytes, offset + 16);
    entry.compressed_size = u32(bytes, offset + 20);
    entry.uncompressed_size = u32(bytes, offset + 24);
    entry.local_offset = u32(bytes, offset + 42);
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

bool inflate_raw(const std::uint8_t *source, std::size_t source_size, std::uint8_t *destination,
                 std::size_t destination_size) {
  if (source_size > std::numeric_limits<uInt>::max() ||
      destination_size > std::numeric_limits<uInt>::max())
    return false;
  std::uint8_t empty_output = 0;
  z_stream stream{};
  stream.next_in = const_cast<Bytef *>(source);
  stream.avail_in = static_cast<uInt>(source_size);
  stream.next_out = destination_size == 0 ? &empty_output : destination;
  stream.avail_out = destination_size == 0 ? 1 : static_cast<uInt>(destination_size);
  if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
    return false;
  const int result = inflate(&stream, Z_FINISH);
  const bool complete = result == Z_STREAM_END && stream.total_in == source_size &&
                        stream.total_out == destination_size;
  inflateEnd(&stream);
  return complete;
}

bool local_data_offset(ByteView bytes, const Entry &entry, std::size_t &data_offset,
                       std::string &error) {
  if (!has(bytes, entry.local_offset, 30) || u32(bytes, entry.local_offset) != 0x04034b50) {
    error = "archive local entry is invalid";
    return false;
  }
  const std::uint16_t local_flags = u16(bytes, entry.local_offset + 6);
  const std::size_t name_size = u16(bytes, entry.local_offset + 26);
  const std::size_t extra_size = u16(bytes, entry.local_offset + 28);
  data_offset = entry.local_offset + 30 + name_size + extra_size;
  const bool descriptor = (entry.flags & 8U) != 0;
  const auto agrees_or_descriptor_zero = [descriptor](std::uint32_t local, std::uint32_t central) {
    return local == central || (descriptor && local == 0);
  };
  if (!has(bytes, entry.local_offset + 30, name_size + extra_size) ||
      std::string_view(reinterpret_cast<const char *>(&bytes[entry.local_offset + 30]),
                       name_size) != entry.name ||
      local_flags != entry.flags || u16(bytes, entry.local_offset + 8) != entry.method ||
      !agrees_or_descriptor_zero(u32(bytes, entry.local_offset + 14), entry.crc) ||
      !agrees_or_descriptor_zero(u32(bytes, entry.local_offset + 18), entry.compressed_size) ||
      !agrees_or_descriptor_zero(u32(bytes, entry.local_offset + 22), entry.uncompressed_size)) {
    error = "archive local entry disagrees with its central directory";
    return false;
  }
  if (!has(bytes, data_offset, entry.compressed_size)) {
    error = "archive entry data is truncated";
    return false;
  }
  return true;
}

bool unpack_entry(ByteView bytes, const Entry &entry, Bytes &unpacked, std::string &error) {
  if ((entry.flags & 1U) != 0 || (entry.method != 0 && entry.method != 8)) {
    error = "archive entry is encrypted or uses an unsupported compression method";
    return false;
  }
  std::size_t data_offset = 0;
  if (!local_data_offset(bytes, entry, data_offset, error))
    return false;
  if (entry.method == 0 && entry.compressed_size != entry.uncompressed_size) {
    error = "stored archive entry has inconsistent sizes";
    return false;
  }
  unpacked.resize(entry.uncompressed_size);
  if (entry.method == 0) {
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(data_offset), entry.uncompressed_size,
                unpacked.begin());
  } else if (!inflate_raw(bytes.data() + data_offset, entry.compressed_size, unpacked.data(),
                          unpacked.size())) {
    error = "archive entry failed deflate decompression";
    return false;
  }
  if (crc32(0, unpacked.data(), unpacked.size()) != entry.crc) {
    error = "archive entry failed CRC validation";
    return false;
  }
  return true;
}

bool validate_entries(ByteView bytes, const std::vector<Entry> &archive_entries,
                      std::string &error) {
  Bytes unpacked;
  for (const Entry &entry : archive_entries) {
    if (!unpack_entry(bytes, entry, unpacked, error))
      return false;
  }
  return true;
}

bool write_entries(ByteView bytes, const std::vector<Entry> &archive_entries,
                   const std::filesystem::path &staging, std::string &error) {
  std::error_code filesystem_error;
  Bytes unpacked;
  for (const Entry &entry : archive_entries) {
    const std::filesystem::path output_path = staging / std::filesystem::path(entry.name);
    if (entry.name.back() == '/') {
      std::filesystem::create_directories(output_path, filesystem_error);
      if (filesystem_error) {
        error = "could not create extracted archive directory: " + output_path.string();
        return false;
      }
      continue;
    }
    if (!unpack_entry(bytes, entry, unpacked, error))
      return false;
    std::filesystem::create_directories(output_path.parent_path(), filesystem_error);
    if (filesystem_error) {
      error = "could not create extracted archive directory: " + output_path.parent_path().string();
      return false;
    }
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
      error = "could not create extracted archive file: " + output_path.string();
      return false;
    }
    if (!unpacked.empty())
      output.write(reinterpret_cast<const char *>(unpacked.data()), unpacked.size());
    if (!output) {
      error = "could not write extracted archive file: " + output_path.string();
      return false;
    }
  }
  return true;
}

void discard_staging(const std::filesystem::path &staging, std::string &error) {
  std::error_code cleanup_error;
  std::filesystem::remove_all(staging, cleanup_error);
  if (cleanup_error)
    error += "; additionally could not remove extraction staging directory: " + staging.string();
}

bool extract_atomically(ByteView bytes, const std::vector<Entry> &archive_entries,
                        const std::filesystem::path &destination,
                        std::vector<std::filesystem::path> &files, std::string &error) {
  if (!validate_entries(bytes, archive_entries, error))
    return false;

  const std::filesystem::path parent =
      destination.parent_path().empty() ? std::filesystem::path{"."} : destination.parent_path();
  std::error_code filesystem_error;
  if (!std::filesystem::is_directory(parent, filesystem_error) || filesystem_error) {
    error = "extraction destination parent does not exist: " + parent.string();
    return false;
  }
  if (std::filesystem::exists(destination, filesystem_error) || filesystem_error) {
    error = filesystem_error ? "could not inspect extraction destination"
                             : "extraction destination already exists";
    return false;
  }
  std::filesystem::path staging = destination;
  staging += ".lucent-stage";
  if (std::filesystem::exists(staging, filesystem_error) || filesystem_error) {
    error = filesystem_error ? "could not inspect extraction staging path"
                             : "extraction staging path already exists: " + staging.string();
    return false;
  }
  if (!std::filesystem::create_directory(staging, filesystem_error) || filesystem_error) {
    error = "could not create extraction staging directory: " + staging.string();
    return false;
  }
  if (!write_entries(bytes, archive_entries, staging, error)) {
    discard_staging(staging, error);
    return false;
  }
  std::filesystem::rename(staging, destination, filesystem_error);
  if (filesystem_error) {
    error = "could not publish extracted archive: " + destination.string();
    discard_staging(staging, error);
    return false;
  }

  std::vector<std::filesystem::path> published;
  for (const Entry &entry : archive_entries) {
    if (entry.name.back() != '/')
      published.push_back(destination / std::filesystem::path(entry.name));
  }
  files.swap(published);
  return true;
}

bool zip_candidate(const Entry &entry, ByteView content) {
  if (equal_name(std::filesystem::path(entry.name).extension().string(), ".zip"))
    return true;
  return content.size() >= 4 && content[0] == 'P' && content[1] == 'K' &&
         ((content[2] == 3 && content[3] == 4) || (content[2] == 5 && content[3] == 6));
}

bool match_content(const ContentMatcher &matches, const Entry &entry, ByteView content,
                   Candidate &candidate, bool nested, std::string &error) {
  bool matched = false;
  try {
    matched = matches(entry.name, content);
  } catch (const std::exception &exception) {
    error = "content matcher failed for " + entry.name + ": " + exception.what();
    return false;
  }
  if (!matched)
    return true;
  if (candidate.entry != nullptr) {
    error = "more than one archive entry matched the required content identity";
    return false;
  }
  candidate = {.entry = &entry, .nested = nested};
  return true;
}

bool inspect_inner_archive(ByteView inner_bytes, const ContentMatcher &matches,
                           const ExtractionLimits &limits, Budget &budget,
                           std::vector<Entry> &inner_entries, Candidate &candidate,
                           std::string &error) {
  if (!entries(inner_bytes, inner_entries, limits, budget, error))
    return false;
  Bytes content;
  for (const Entry &entry : inner_entries) {
    if (!unpack_entry(inner_bytes, entry, content, error))
      return false;
    if (entry.name.back() == '/')
      continue;
    if (zip_candidate(entry, content)) {
      error = "archive contains a ZIP nested more than one level deep: " + entry.name;
      return false;
    }
    if (!match_content(matches, entry, content, candidate, true, error))
      return false;
  }
  return true;
}

bool extract_unique_install_impl(ByteView archive, const std::filesystem::path &destination,
                                 const ContentMatcher &matches, std::filesystem::path &matched_file,
                                 std::string &error, const ExtractionLimits &limits) {
  if (!matches) {
    error = "archive content matcher is empty";
    return false;
  }
  Budget budget;
  std::vector<Entry> outer_entries;
  if (!entries(archive, outer_entries, limits, budget, error))
    return false;

  Candidate candidate;
  Bytes content;
  Bytes inner_bytes;
  std::vector<Entry> inner_entries;
  bool found_nested_archive = false;
  for (const Entry &entry : outer_entries) {
    if (!unpack_entry(archive, entry, content, error))
      return false;
    if (entry.name.back() == '/')
      continue;
    if (!zip_candidate(entry, content)) {
      if (!match_content(matches, entry, content, candidate, false, error))
        return false;
      continue;
    }
    if (found_nested_archive) {
      error = "archive contains more than one nested ZIP";
      return false;
    }
    found_nested_archive = true;
    inner_bytes = content;
    if (!inspect_inner_archive(inner_bytes, matches, limits, budget, inner_entries, candidate,
                               error)) {
      error.insert(0, ": ");
      error.insert(0, entry.name);
      error.insert(0, "nested ZIP ");
      return false;
    }
  }
  if (candidate.entry == nullptr) {
    error = "no archive entry matched the required content identity";
    return false;
  }

  std::vector<std::filesystem::path> files;
  const ByteView selected_bytes = candidate.nested ? ByteView{inner_bytes} : archive;
  const std::vector<Entry> &selected_entries = candidate.nested ? inner_entries : outer_entries;
  const std::string selected_name = candidate.entry->name;
  if (!extract_atomically(selected_bytes, selected_entries, destination, files, error))
    return false;
  matched_file = destination / std::filesystem::path(selected_name);
  return true;
}

} // namespace

bool extract_archive(const std::filesystem::path &archive, const std::filesystem::path &destination,
                     std::vector<std::filesystem::path> &files, std::string &error,
                     ExtractionLimits limits) {
  MappedArchive mapped;
  std::string failure;
  if (!mapped.open(archive, limits, failure) ||
      !extract_archive(mapped.bytes(), destination, files, failure, limits)) {
    error = std::move(failure);
    return false;
  }
  error.clear();
  return true;
}

bool extract_archive(std::span<const std::uint8_t> archive,
                     const std::filesystem::path &destination,
                     std::vector<std::filesystem::path> &files, std::string &error,
                     ExtractionLimits limits) {
  Budget budget;
  std::vector<Entry> archive_entries;
  std::vector<std::filesystem::path> published;
  std::string failure;
  if (!entries(archive, archive_entries, limits, budget, failure) ||
      !extract_atomically(archive, archive_entries, destination, published, failure)) {
    error = std::move(failure);
    return false;
  }
  files.swap(published);
  error.clear();
  return true;
}

bool find_unique_file(const std::vector<std::filesystem::path> &files, const FileMatcher &matches,
                      std::filesystem::path &file, std::string &error) {
  if (!matches) {
    error = "extracted-file matcher is empty";
    return false;
  }
  std::filesystem::path selected;
  try {
    for (const std::filesystem::path &candidate : files) {
      if (!matches(candidate))
        continue;
      if (!selected.empty()) {
        error = "more than one extracted file matched the required identity";
        return false;
      }
      selected = candidate;
    }
  } catch (const std::exception &exception) {
    error = "extracted-file matcher failed: " + std::string{exception.what()};
    return false;
  }
  if (selected.empty()) {
    error = "no extracted file matched the required identity";
    return false;
  }
  file = std::move(selected);
  error.clear();
  return true;
}

bool extract_unique_install(const std::filesystem::path &archive,
                            const std::filesystem::path &destination, const ContentMatcher &matches,
                            std::filesystem::path &matched_file, std::string &error,
                            ExtractionLimits limits) {
  MappedArchive mapped;
  std::string failure;
  std::filesystem::path selected = matched_file;
  if (!mapped.open(archive, limits, failure) ||
      !extract_unique_install_impl(mapped.bytes(), destination, matches, selected, failure,
                                   limits)) {
    error = std::move(failure);
    return false;
  }
  matched_file = std::move(selected);
  error.clear();
  return true;
}

bool extract_unique_install(std::span<const std::uint8_t> archive,
                            const std::filesystem::path &destination, const ContentMatcher &matches,
                            std::filesystem::path &matched_file, std::string &error,
                            ExtractionLimits limits) {
  std::string failure;
  std::filesystem::path selected = matched_file;
  if (!extract_unique_install_impl(archive, destination, matches, selected, failure, limits)) {
    error = std::move(failure);
    return false;
  }
  matched_file = std::move(selected);
  error.clear();
  return true;
}

bool extract_install(const std::filesystem::path &archive, const std::filesystem::path &destination,
                     std::string_view required_name, std::filesystem::path &executable,
                     std::string &error, ExtractionLimits limits) {
  MappedArchive mapped;
  std::string failure;
  if (!mapped.open(archive, limits, failure)) {
    error = std::move(failure);
    return false;
  }
  Budget budget;
  std::vector<Entry> archive_entries;
  if (!entries(mapped.bytes(), archive_entries, limits, budget, failure)) {
    error = std::move(failure);
    return false;
  }
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
  std::vector<std::filesystem::path> files;
  if (!extract_atomically(mapped.bytes(), archive_entries, destination, files, failure)) {
    error = std::move(failure);
    return false;
  }
  executable = *std::find_if(files.begin(), files.end(), [required_name](const auto &file) {
    return equal_name(file.filename().string(), required_name);
  });
  error.clear();
  return true;
}

} // namespace lucent::zip

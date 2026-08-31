#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lucent::zip {

struct ExtractionLimits {
  // Combined byte budget for the outer archive and its optional nested archive.
  std::uint64_t max_archive_bytes = 512ULL * 1024ULL * 1024ULL;
  // Combined expanded-byte budget across both archive levels.
  std::uint64_t max_extracted_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
  std::uint64_t max_entry_bytes = 512ULL * 1024ULL * 1024ULL;
  // Combined entry-count budget across both archive levels.
  std::size_t max_entries = 50000;
};

using FileMatcher = std::function<bool(const std::filesystem::path &)>;
using ContentMatcher = std::function<bool(std::string_view, std::span<const std::uint8_t>)>;

// Extract every regular file from a validated archive. The returned paths are inside destination
// and preserve their archive-relative layout. The caller supplies a fresh destination so it can
// validate title-specific identity and atomically accept or discard the complete preparation.
bool extract_archive(const std::filesystem::path &archive, const std::filesystem::path &destination,
                     std::vector<std::filesystem::path> &files, std::string &error,
                     ExtractionLimits limits = {});

// Byte-input counterpart to extract_archive. Neither overload changes files or destination on
// failure. Destination must not already exist; extraction is prepared beside it and published only
// after every entry has passed structural, decompression, and CRC validation.
bool extract_archive(std::span<const std::uint8_t> archive,
                     const std::filesystem::path &destination,
                     std::vector<std::filesystem::path> &files, std::string &error,
                     ExtractionLimits limits = {});

// Select exactly one extracted file using consumer-owned identity policy. The matcher can inspect
// file content; Lucent owns only the zero/ambiguous-match contract.
bool find_unique_file(const std::vector<std::filesystem::path> &files, const FileMatcher &matches,
                      std::filesystem::path &file, std::string &error);

// Search an outer ZIP and at most one ZIP contained directly within it. Every regular entry is
// decompressed and CRC-checked before matching. The matcher receives the archive-relative name and
// a view valid for that call, allowing title identity to be checked from content rather than
// archive layout. Exactly one entry must match across both levels. On success, the archive level
// containing that entry is extracted atomically to destination and matched_file names the published
// file. Neither destination nor matched_file changes on failure.
bool extract_unique_install(const std::filesystem::path &archive,
                            const std::filesystem::path &destination, const ContentMatcher &matches,
                            std::filesystem::path &matched_file, std::string &error,
                            ExtractionLimits limits = {});
bool extract_unique_install(std::span<const std::uint8_t> archive,
                            const std::filesystem::path &destination, const ContentMatcher &matches,
                            std::filesystem::path &matched_file, std::string &error,
                            ExtractionLimits limits = {});

// Extract an install archive and locate exactly one file whose basename matches required_name.
// Paths are validated against traversal/absolute names before any output is written. The caller
// supplies a fresh destination and receives the extracted executable's path.
bool extract_install(const std::filesystem::path &archive, const std::filesystem::path &destination,
                     std::string_view required_name, std::filesystem::path &executable,
                     std::string &error, ExtractionLimits limits = {});

} // namespace lucent::zip

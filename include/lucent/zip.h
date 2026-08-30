#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lucent::zip {

struct ExtractionLimits {
  std::uint64_t max_archive_bytes = 512ULL * 1024ULL * 1024ULL;
  std::uint64_t max_extracted_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
  std::uint64_t max_entry_bytes = 512ULL * 1024ULL * 1024ULL;
  std::size_t max_entries = 50000;
};

using FileMatcher = std::function<bool(const std::filesystem::path &)>;

// Extract every regular file from a validated archive. The returned paths are inside destination
// and preserve their archive-relative layout. The caller supplies a fresh destination so it can
// validate title-specific identity and atomically accept or discard the complete preparation.
bool extract_archive(const std::filesystem::path &archive, const std::filesystem::path &destination,
                     std::vector<std::filesystem::path> &files, std::string &error,
                     ExtractionLimits limits = {});

// Select exactly one extracted file using consumer-owned identity policy. The matcher can inspect
// file content; Lucent owns only the zero/ambiguous-match contract.
bool find_unique_file(const std::vector<std::filesystem::path> &files, const FileMatcher &matches,
                      std::filesystem::path &file, std::string &error);

// Extract an install archive and locate exactly one file whose basename matches required_name.
// Paths are validated against traversal/absolute names before any output is written. The caller
// supplies a fresh destination and receives the extracted executable's path.
bool extract_install(const std::filesystem::path &archive, const std::filesystem::path &destination,
                     std::string_view required_name, std::filesystem::path &executable,
                     std::string &error, ExtractionLimits limits = {});

} // namespace lucent::zip

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace lucent::zip {

struct ExtractionLimits {
  std::uint64_t max_archive_bytes = 512ULL * 1024ULL * 1024ULL;
  std::uint64_t max_extracted_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
  std::uint64_t max_entry_bytes = 512ULL * 1024ULL * 1024ULL;
  std::size_t max_entries = 50000;
};

// Extract an install archive and locate exactly one file whose basename matches required_name.
// Paths are validated against traversal/absolute names before any output is written. The caller
// supplies a fresh destination and receives the extracted executable's path.
bool extract_install(const std::filesystem::path &archive, const std::filesystem::path &destination,
                     std::string_view required_name, std::filesystem::path &executable,
                     std::string &error, ExtractionLimits limits = {});

} // namespace lucent::zip

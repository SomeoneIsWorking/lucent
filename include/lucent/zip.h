#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace lucent::zip {

// Extract an install archive and locate exactly one file whose basename matches required_name.
// Paths are validated against traversal/absolute names before any output is written. The caller
// supplies a fresh destination and receives the extracted executable's path.
bool extract_install(const std::filesystem::path &archive, const std::filesystem::path &destination,
                     std::string_view required_name, std::filesystem::path &executable,
                     std::string &error);

} // namespace lucent::zip

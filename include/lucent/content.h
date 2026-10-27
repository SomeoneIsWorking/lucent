#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace lucent::content {

using Sha256 = std::array<std::uint8_t, 32>;
using Sha512 = std::array<std::uint8_t, 64>;

// Hash an in-memory byte sequence using SHA-256.
Sha256 sha256(std::span<const std::byte> bytes);

// Stream a file through SHA-256 without loading it into memory. Failures carry a diagnostic and
// never return a partial digest.
std::optional<Sha256> sha256_file(const std::filesystem::path &path, std::string &error);

// Return the canonical lower-case hexadecimal representation.
std::string sha256_hex(const Sha256 &digest);

// Hash an in-memory byte sequence using SHA-512.
Sha512 sha512(std::span<const std::byte> bytes);

// Stream a file through SHA-512, as sha256_file does.
std::optional<Sha512> sha512_file(const std::filesystem::path &path, std::string &error);

// Return the canonical lower-case hexadecimal representation.
std::string sha512_hex(const Sha512 &digest);

} // namespace lucent::content

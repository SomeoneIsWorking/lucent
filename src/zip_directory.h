#pragma once

#include "lucent/zip.h"
#include "zip_reader.h"

namespace lucent::zip::detail {
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
bool equal_name(std::string_view left, std::string_view right);
bool entries(ArchiveReader &archive, std::vector<Entry> &out, const ExtractionLimits &limits,
             Budget &budget, std::string &error);
bool local_data_offset(ArchiveReader &archive, const Entry &entry, std::uint64_t &data_offset,
                       std::string &error);
} // namespace lucent::zip::detail

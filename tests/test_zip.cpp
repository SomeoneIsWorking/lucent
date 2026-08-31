#include "lucent/zip.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <zlib.h>

namespace {

void u16(std::vector<unsigned char> &bytes, unsigned value) {
  bytes.push_back(static_cast<unsigned char>(value));
  bytes.push_back(static_cast<unsigned char>(value >> 8));
}

void u32(std::vector<unsigned char> &bytes, unsigned value) {
  u16(bytes, value);
  u16(bytes, value >> 16);
}

std::vector<unsigned char> deflate_raw(std::string_view input) {
  std::vector<unsigned char> output(compressBound(input.size()));
  z_stream stream{};
  stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
  stream.avail_in = static_cast<uInt>(input.size());
  stream.next_out = output.data();
  stream.avail_out = static_cast<uInt>(output.size());
  if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) !=
          Z_OK ||
      deflate(&stream, Z_FINISH) != Z_STREAM_END) {
    return {};
  }
  output.resize(stream.total_out);
  deflateEnd(&stream);
  return output;
}

void entry(std::vector<unsigned char> &archive, std::vector<unsigned char> &central,
           std::string_view name, std::string_view content, unsigned method) {
  const auto compressed = method == 8 ? deflate_raw(content)
                                      : std::vector<unsigned char>(content.begin(), content.end());
  const unsigned crc = crc32(0, reinterpret_cast<const Bytef *>(content.data()), content.size());
  const unsigned local_offset = archive.size();
  u32(archive, 0x04034b50);
  u16(archive, 20);
  u16(archive, 0);
  u16(archive, method);
  u16(archive, 0);
  u16(archive, 0);
  u32(archive, crc);
  u32(archive, compressed.size());
  u32(archive, content.size());
  u16(archive, name.size());
  u16(archive, 0);
  archive.insert(archive.end(), name.begin(), name.end());
  archive.insert(archive.end(), compressed.begin(), compressed.end());

  u32(central, 0x02014b50);
  u16(central, 20);
  u16(central, 20);
  u16(central, 0);
  u16(central, method);
  u16(central, 0);
  u16(central, 0);
  u32(central, crc);
  u32(central, compressed.size());
  u32(central, content.size());
  u16(central, name.size());
  u16(central, 0);
  u16(central, 0);
  u16(central, 0);
  u16(central, 0);
  u32(central, 0);
  u32(central, local_offset);
  central.insert(central.end(), name.begin(), name.end());
}

struct FixtureEntry {
  std::string name;
  std::string content;
  unsigned method = 0;
};

std::vector<unsigned char> make_archive(std::initializer_list<FixtureEntry> entries) {
  std::vector<unsigned char> archive;
  std::vector<unsigned char> central;
  for (const auto &fixture : entries)
    entry(archive, central, fixture.name, fixture.content, fixture.method);
  const unsigned central_offset = archive.size();
  archive.insert(archive.end(), central.begin(), central.end());
  u32(archive, 0x06054b50);
  u16(archive, 0);
  u16(archive, 0);
  u16(archive, entries.size());
  u16(archive, entries.size());
  u32(archive, central.size());
  u32(archive, central_offset);
  u16(archive, 0);
  return archive;
}

std::vector<unsigned char> make_archive() {
  return make_archive({{"Install/readme.txt", "fixture", 0},
                       {"Install/Sub/XMen2.exe", "not a game", 8},
                       {"Install/empty.txt", "", 8}});
}

std::string bytes_string(const std::vector<unsigned char> &bytes) {
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

bool content_is(std::span<const std::uint8_t> content, std::string_view expected) {
  return content.size() == expected.size() &&
         std::equal(content.begin(), content.end(), expected.begin());
}

} // namespace

int main() {
  const std::filesystem::path archive = "zip-test-fixture.zip";
  const std::filesystem::path destination = "zip-test-output";
  {
    std::ofstream output(archive, std::ios::binary);
    const auto bytes = make_archive();
    output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  }
  std::filesystem::path executable;
  std::string error;

  std::vector<std::filesystem::path> extracted_files;
  if (!lucent::zip::extract_archive(archive, destination, extracted_files, error) ||
      extracted_files.size() != 3 || std::filesystem::file_size(extracted_files[2]) != 0) {
    std::cerr << "validated archive was not extracted: " << error << "\n";
    return 1;
  }
  const auto has_identity = [](const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()) ==
           "not a game";
  };
  if (!lucent::zip::find_unique_file(extracted_files, has_identity, executable, error) ||
      executable.filename() != "XMen2.exe") {
    std::cerr << "content identity did not select the nested file: " << error << "\n";
    return 1;
  }
  if (lucent::zip::find_unique_file(
          extracted_files, [](const auto &) { return false; }, executable, error) ||
      error.find("no extracted file") == std::string::npos) {
    std::cerr << "missing identity was not refused: " << error << "\n";
    return 1;
  }
  if (lucent::zip::find_unique_file(
          extracted_files, [](const auto &) { return true; }, executable, error) ||
      error.find("more than one") == std::string::npos) {
    std::cerr << "ambiguous identity was not refused: " << error << "\n";
    return 1;
  }
  if (lucent::zip::find_unique_file(extracted_files, {}, executable, error) ||
      error.find("matcher is empty") == std::string::npos) {
    std::cerr << "empty identity matcher was not refused: " << error << "\n";
    return 1;
  }
  std::filesystem::remove_all(destination);

  const std::filesystem::path mismatch_archive = "zip-test-local-mismatch.zip";
  {
    auto bytes = make_archive();
    bytes[6] = 1;
    std::ofstream output(mismatch_archive, std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  }
  extracted_files.clear();
  if (lucent::zip::extract_archive(mismatch_archive, destination, extracted_files, error) ||
      error.find("disagrees with its central directory") == std::string::npos ||
      std::filesystem::exists(destination)) {
    std::cerr << "local/central mismatch was not refused before staging: " << error << "\n";
    return 1;
  }
  std::filesystem::remove(mismatch_archive);

  if (!lucent::zip::extract_install(archive, destination, "XMen2.exe", executable, error) ||
      executable.filename() != "XMen2.exe" || !std::filesystem::is_regular_file(executable)) {
    std::cerr << "nested executable was not extracted: " << error << "\n";
    return 1;
  }
  std::filesystem::remove_all(destination);

  const auto expect_refused = [&](lucent::zip::ExtractionLimits limits, std::string_view expected) {
    error.clear();
    executable.clear();
    if (lucent::zip::extract_install(archive, destination, "XMen2.exe", executable, error,
                                     limits) ||
        error.find(expected) == std::string::npos || std::filesystem::exists(destination)) {
      std::cerr << "limit refusal failed; expected " << expected << ", got " << error << "\n";
      return false;
    }
    return true;
  };

  lucent::zip::ExtractionLimits limits;
  limits.max_archive_bytes = std::filesystem::file_size(archive) - 1;
  if (!expect_refused(limits, "compressed byte limit"))
    return 1;
  limits = {};
  limits.max_entries = 1;
  if (!expect_refused(limits, "entry-count limit"))
    return 1;
  limits = {};
  limits.max_entry_bytes = 5;
  if (!expect_refused(limits, "entry exceeds"))
    return 1;
  limits = {};
  limits.max_extracted_bytes = 10;
  if (!expect_refused(limits, "total expanded byte limit"))
    return 1;

  const auto inner =
      make_archive({{"Disc/SLUS_010.40", "title identity", 8}, {"Disc/SYSTEM.CNF", "BOOT", 0}});
  const auto outer = make_archive(
      {{"wrapper/readme.txt", "outer", 0}, {"wrapper/media.zip", bytes_string(inner), 8}});
  const auto identity = [](std::string_view, std::span<const std::uint8_t> content) {
    return content_is(content, "title identity");
  };
  const std::filesystem::path byte_destination = "zip-test-byte-output";
  extracted_files.clear();
  if (!lucent::zip::extract_archive(std::span<const std::uint8_t>{inner.data(), inner.size()},
                                    byte_destination, extracted_files, error) ||
      extracted_files.size() != 2) {
    std::cerr << "byte-input archive was not extracted: " << error << "\n";
    return 1;
  }
  std::filesystem::remove_all(byte_destination);
  const std::filesystem::path nested_destination = "zip-test-nested-output";
  executable = "previous-selection";
  if (!lucent::zip::extract_unique_install(
          std::span<const std::uint8_t>{outer.data(), outer.size()}, nested_destination, identity,
          executable, error) ||
      executable != nested_destination / "Disc/SLUS_010.40" ||
      !std::filesystem::is_regular_file(nested_destination / "Disc/SYSTEM.CNF") ||
      std::filesystem::exists(nested_destination / "wrapper/readme.txt")) {
    std::cerr << "nested content install was not selected and published: " << error << "\n";
    return 1;
  }
  std::filesystem::remove_all(nested_destination);

  const std::filesystem::path direct_archive = "zip-test-direct.zip";
  const auto direct =
      make_archive({{"Disc/SLUS_010.40", "title identity", 8}, {"Disc/SYSTEM.CNF", "BOOT", 0}});
  {
    std::ofstream output(direct_archive, std::ios::binary);
    output.write(reinterpret_cast<const char *>(direct.data()), direct.size());
  }
  const std::filesystem::path direct_destination = "zip-test-direct-output";
  if (!lucent::zip::extract_unique_install(direct_archive, direct_destination, identity, executable,
                                           error) ||
      executable != direct_destination / "Disc/SLUS_010.40") {
    std::cerr << "path-input content install was not selected: " << error << "\n";
    return 1;
  }
  std::filesystem::remove_all(direct_destination);
  std::filesystem::remove(direct_archive);

  const auto expect_unique_refused = [&](const std::vector<unsigned char> &bytes,
                                         const lucent::zip::ContentMatcher &matcher,
                                         lucent::zip::ExtractionLimits refusal_limits,
                                         std::string_view expected) {
    const std::filesystem::path refused_destination = "zip-test-refused-output";
    executable = "previous-selection";
    error.clear();
    if (lucent::zip::extract_unique_install(
            std::span<const std::uint8_t>{bytes.data(), bytes.size()}, refused_destination, matcher,
            executable, error, refusal_limits) ||
        error.find(expected) == std::string::npos || executable != "previous-selection" ||
        std::filesystem::exists(refused_destination)) {
      std::cerr << "nested ZIP refusal failed; expected " << expected << ", got " << error << "\n";
      return false;
    }
    return true;
  };

  const auto duplicate_identity =
      make_archive({{"direct.bin", "title identity", 0}, {"nested.zip", bytes_string(inner), 0}});
  if (!expect_unique_refused(duplicate_identity, identity, {}, "more than one archive entry"))
    return 1;
  if (!expect_unique_refused(
          outer, [](std::string_view, auto) { return false; }, {}, "no archive entry"))
    return 1;
  if (!expect_unique_refused(outer, {}, {}, "matcher is empty"))
    return 1;

  const auto unsafe_inner = make_archive({{"../SLUS_010.40", "title identity", 0}});
  const auto unsafe_outer = make_archive({{"unsafe.zip", bytes_string(unsafe_inner), 0}});
  if (!expect_unique_refused(unsafe_outer, identity, {}, "unsafe path"))
    return 1;
  const auto aliased_inner =
      make_archive({{"Disc/game.bin", "title identity", 0}, {"disc/GAME.BIN", "other", 0}});
  const auto aliased_outer = make_archive({{"aliases.zip", bytes_string(aliased_inner), 0}});
  if (!expect_unique_refused(aliased_outer, identity, {}, "duplicate output paths"))
    return 1;
  const auto device_inner = make_archive({{"Disc/CON.txt", "title identity", 0}});
  const auto device_outer = make_archive({{"device.zip", bytes_string(device_inner), 0}});
  if (!expect_unique_refused(device_outer, identity, {}, "unsafe path"))
    return 1;

  auto corrupt_inner = make_archive({{"Disc/SLUS_010.40", "title identity", 0}});
  corrupt_inner[30 + std::string_view{"Disc/SLUS_010.40"}.size()] ^= 0xff;
  const auto corrupt_outer = make_archive({{"corrupt.zip", bytes_string(corrupt_inner), 0}});
  if (!expect_unique_refused(corrupt_outer, identity, {}, "CRC validation"))
    return 1;

  const auto deepest = make_archive({{"game.bin", "title identity", 0}});
  const auto middle = make_archive({{"again.zip", bytes_string(deepest), 0}});
  const auto too_deep = make_archive({{"inside.zip", bytes_string(middle), 0}});
  if (!expect_unique_refused(too_deep, identity, {}, "more than one level deep"))
    return 1;

  const auto two_nested =
      make_archive({{"one.zip", bytes_string(inner), 0}, {"two.zip", bytes_string(deepest), 0}});
  if (!expect_unique_refused(two_nested, identity, {}, "more than one nested ZIP"))
    return 1;

  limits = {};
  limits.max_entries = 3;
  if (!expect_unique_refused(outer, identity, limits, "combined entry-count limit"))
    return 1;
  limits = {};
  limits.max_archive_bytes = outer.size() + inner.size() - 1;
  if (!expect_unique_refused(outer, identity, limits, "combined compressed byte limit"))
    return 1;
  limits = {};
  limits.max_extracted_bytes = std::string_view{"outer"}.size() + inner.size();
  if (!expect_unique_refused(outer, identity, limits, "total expanded byte limit"))
    return 1;

  const std::filesystem::path preserved_destination = "zip-test-preserved-output";
  std::filesystem::create_directory(preserved_destination);
  {
    std::ofstream marker(preserved_destination / "valid-selection");
    marker << "keep";
  }
  executable = "previous-selection";
  if (lucent::zip::extract_unique_install(
          std::span<const std::uint8_t>{direct.data(), direct.size()}, preserved_destination,
          identity, executable, error) ||
      error.find("destination already exists") == std::string::npos ||
      executable != "previous-selection" ||
      !std::filesystem::is_regular_file(preserved_destination / "valid-selection")) {
    std::cerr << "existing valid selection was not preserved: " << error << "\n";
    return 1;
  }
  std::filesystem::remove_all(preserved_destination);

  if (!expect_unique_refused(
          outer,
          [](std::string_view, std::span<const std::uint8_t>) -> bool {
            throw std::runtime_error("identity reader failed");
          },
          {}, "identity reader failed"))
    return 1;

  std::filesystem::remove(archive);
  std::cout << "zip: direct and one-level nested content identity selected from path/bytes; "
               "atomic publication, corruption, ambiguity, depth, path, and aggregate budgets "
               "validated\n";
  return 0;
}

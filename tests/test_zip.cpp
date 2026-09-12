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

#include "zip_fixture.h"
using namespace zip_test;

int main() {
  const std::filesystem::path archive = "zip-test-fixture.zip";
  const std::filesystem::path destination = "zip-test-output";
  const std::filesystem::path staging = "zip-test-output.lucent-stage";
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
      std::filesystem::exists(destination) || std::filesystem::exists(staging)) {
    std::cerr << "local/central mismatch left a published or staged extraction: " << error << "\n";
    return 1;
  }
  std::filesystem::remove(mismatch_archive);

  const std::filesystem::path corrupt_archive = "zip-test-corrupt-after-first.zip";
  {
    auto bytes = make_archive();
    const std::string name = "Install/Sub/XMen2.exe";
    const auto local_name = std::search(bytes.begin(), bytes.end(), name.begin(), name.end());
    if (local_name == bytes.end() ||
        static_cast<std::size_t>(bytes.end() - local_name) <= name.size()) {
      std::cerr << "could not locate the fixture's second entry payload\n";
      return 1;
    }
    *(local_name + static_cast<std::ptrdiff_t>(name.size())) ^= 1;
    std::ofstream output(corrupt_archive, std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  }
  extracted_files.clear();
  if (lucent::zip::extract_archive(corrupt_archive, destination, extracted_files, error) ||
      error.empty() || std::filesystem::exists(destination) || std::filesystem::exists(staging) ||
      !extracted_files.empty()) {
    std::cerr << "corruption after a valid entry left a published or staged extraction: " << error
              << "\n";
    return 1;
  }
  std::filesystem::remove(corrupt_archive);

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

#include "lucent/zip.h"

#include <fstream>
#include <iostream>
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

std::vector<unsigned char> make_archive() {
  std::vector<unsigned char> archive;
  std::vector<unsigned char> central;
  entry(archive, central, "Install/readme.txt", "fixture", 0);
  entry(archive, central, "Install/Sub/XMen2.exe", "not a game", 8);
  const unsigned central_offset = archive.size();
  archive.insert(archive.end(), central.begin(), central.end());
  u32(archive, 0x06054b50);
  u16(archive, 0);
  u16(archive, 0);
  u16(archive, 2);
  u16(archive, 2);
  u32(archive, central.size());
  u32(archive, central_offset);
  u16(archive, 0);
  return archive;
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

  std::filesystem::remove(archive);
  std::cout
      << "zip: nested install extracted; archive, entry, count, and expansion limits refused\n";
  return 0;
}

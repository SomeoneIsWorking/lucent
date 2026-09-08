#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include <zlib.h>

namespace zip_test {

inline void u16(std::vector<unsigned char> &bytes, unsigned value) {
  bytes.push_back(static_cast<unsigned char>(value));
  bytes.push_back(static_cast<unsigned char>(value >> 8));
}

inline void u32(std::vector<unsigned char> &bytes, unsigned value) {
  u16(bytes, value);
  u16(bytes, value >> 16);
}

inline std::vector<unsigned char> deflate_raw(std::string_view input) {
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

inline void entry(std::vector<unsigned char> &archive, std::vector<unsigned char> &central,
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

inline std::vector<unsigned char> make_archive(std::initializer_list<FixtureEntry> entries) {
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

inline std::vector<unsigned char> make_archive() {
  return make_archive({{"Install/readme.txt", "fixture", 0},
                       {"Install/Sub/XMen2.exe", "not a game", 8},
                       {"Install/empty.txt", "", 8}});
}

inline std::string bytes_string(const std::vector<unsigned char> &bytes) {
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

inline bool content_is(std::span<const std::uint8_t> content, std::string_view expected) {
  return content.size() == expected.size() &&
         std::equal(content.begin(), content.end(), expected.begin(),
                    [](std::uint8_t byte, char value) {
                      return byte == static_cast<std::uint8_t>(value);
                    });
}

} // namespace zip_test

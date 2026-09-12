#include "lucent/zip.h"
#include "zip_directory.h"
#include "zip_entry.h"
#include "zip_fixture.h"
#include "zip_reader.h"

#include <array>
#include <fstream>
#include <iostream>
#include <limits>

namespace {
using namespace lucent::zip;
using namespace lucent::zip::detail;
using namespace zip_test;
unsigned checks, failures;
void check(bool passed, std::string_view message) {
  checks++;
  if (!passed) {
    failures++;
    std::cerr << "FAIL: " << message << '\n';
  }
}

class ObservedArchive final : public ArchiveReader {
public:
  explicit ObservedArchive(ArchiveReader &source) : source_(source) {}
  std::uint64_t size() const override { return source_.size(); }
  std::size_t calls = 0, largest = 0;
  std::uint64_t bytes = 0;
  std::uint64_t readable_size = std::numeric_limits<std::uint64_t>::max();

private:
  bool read_chunk(std::uint64_t offset, std::span<std::uint8_t> output,
                  std::string &error) override {
    calls++;
    largest = std::max(largest, output.size());
    bytes += output.size();
    if (offset > readable_size || output.size() > readable_size - offset) {
      error = "fixture backing archive was truncated";
      return false;
    }
    return source_.read(offset, output, error);
  }
  ArchiveReader &source_;
};

void stream_fixture(unsigned method) {
  std::string content(5 * read_chunk_bytes + 13, '\0');
  std::uint32_t random = 1;
  for (char &byte : content) {
    random = random * 1664525u + 1013904223u;
    byte = static_cast<char>(random >> 24);
  }
  const auto fixture = make_archive({{"payload.bin", content, method}});
  SpanArchive span(fixture);
  ObservedArchive archive(span);
  std::vector<Entry> directory;
  Budget budget;
  std::string error;
  check(entries(archive, directory, {}, budget, error) && directory.size() == 1,
        "production parser reads streaming fixture");
  if (directory.empty())
    return;
  std::size_t emitted = 0, largest_output = 0;
  const auto receive = [&](ByteView chunk) {
    largest_output = std::max(largest_output, chunk.size());
    const bool equal = content_is(chunk, std::string_view(content).substr(emitted, chunk.size()));
    emitted += chunk.size();
    return equal;
  };
  check(stream_entry(archive, directory[0], receive, error) && emitted == content.size(),
        "streamed stored/deflate bytes match the original content");
  check(archive.calls > 5 && archive.largest <= read_chunk_bytes &&
            largest_output <= read_chunk_bytes,
        "archive input and expanded output use bounded chunks");
  check(!stream_entry(
            archive, directory[0], [](ByteView) { return false; }, error) &&
            error.find("could not write") != std::string::npos,
        "output failure is propagated");
  archive.readable_size = 30 + directory[0].name.size() + directory[0].compressed_size - 1;
  check(!stream_entry(archive, directory[0], {}, error) &&
            error.find("truncated") != std::string::npos,
        "truncation after directory inspection fails exact reads");
  archive.readable_size = archive.size();
  Entry bad = directory[0];
  bad.crc ^= 1;
  // Update both headers to pass structural agreement and reach the CRC owner.
  auto corrupted = fixture;
  corrupted[14] ^= 1;
  SpanArchive corrupt_span(corrupted);
  check(!stream_entry(corrupt_span, bad, {}, error) &&
            error.find("CRC validation") != std::string::npos,
        "streaming CRC rejects corrupt content");
}

void buffered_directory_fixture() {
  const auto fixture = make_archive();
  SpanArchive span(fixture);
  ObservedArchive archive(span);
  std::vector<Entry> directory;
  Budget budget;
  std::string error;
  check(entries(archive, directory, {}, budget, error) && directory.size() == 3,
        "production parser accepts the complete install directory");
  check(archive.calls == 2 && archive.bytes < 2 * read_chunk_bytes,
        "central metadata needs one bounded read, not per-entry seeks");
}

void large_sparse_archive() {
  const std::filesystem::path path = "zip-stream-sparse.zip";
  const std::filesystem::path destination = "zip-stream-output";
  constexpr std::uint64_t central_at = 1536ULL * 1024 * 1024;
  std::vector<unsigned char> local, central;
  entry(local, central, "deep/Game.exe", "streamed identity", 8);
  std::vector<unsigned char> end;
  u32(end, 0x06054b50);
  u16(end, 0);
  u16(end, 0);
  u16(end, 1);
  u16(end, 1);
  u32(end, central.size());
  u32(end, central_at);
  u16(end, 0);
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char *>(local.data()), local.size());
    output.seekp(static_cast<std::streamoff>(central_at));
    output.write(reinterpret_cast<const char *>(central.data()), central.size());
    output.write(reinterpret_cast<const char *>(end.data()), end.size());
    check(static_cast<bool>(output), "creates sparse 1.5GiB archive fixture");
  }
  ExtractionLimits limits;
  limits.max_archive_bytes = central_at + central.size() + end.size();
  {
    FileArchive file;
    std::string error;
    check(file.open(path, limits, error), "opens large archive without mapping it");
    ObservedArchive archive(file);
    std::vector<Entry> directory;
    Budget budget;
    check(entries(archive, directory, limits, budget, error) && directory.size() == 1,
          "reads central directory beyond 1.5GiB through production reader");
    if (!directory.empty())
      check(stream_entry(archive, directory[0], {}, error), "validates sparse archive entry");
    check(archive.bytes < 128 * 1024 && archive.largest <= read_chunk_bytes,
          "sparse large archive inspects less than 128KiB in bounded reads");
    std::filesystem::path selected = "previous-selection";
    check(extract_install(path, destination, "Game.exe", selected, error, limits) &&
              selected == destination / "deep/Game.exe",
          "shipping filename selection extracts large file-backed archive");
    {
      std::ifstream result(selected, std::ios::binary);
      std::string content((std::istreambuf_iterator<char>(result)), {});
      check(content == "streamed identity", "published large-archive output is correct");
    }
    std::filesystem::remove_all(destination);
    // The open reader retains its original size, so a changed backing file must
    // return a named read failure instead of accepting uninitialized tail bytes.
    std::filesystem::resize_file(path, 8);
    std::array<std::uint8_t, 46> header{};
    check(!file.read(central_at, header, error) && error.find("truncated") != std::string::npos,
          "file reader refuses backing-file truncation");
    check(!file.read(std::numeric_limits<std::uint64_t>::max(), header, error) &&
              error.find("declared size") != std::string::npos,
          "reader rejects overflowing/out-of-range offsets before I/O");
  }
  std::filesystem::remove(path);
}
} // namespace

int main() {
  stream_fixture(0);
  stream_fixture(8);
  buffered_directory_fixture();
  large_sparse_archive();
  std::cout << "zip streaming: " << checks - failures << '/' << checks
            << " checks passed; bounded directory reads, 64KiB read/output bound, "
               "1.5GiB sparse archive, CRC, "
               "sink failure and backing-file truncation exercised\n";
  return failures ? 1 : 0;
}

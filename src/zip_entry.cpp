#include "zip_entry.h"

#include <algorithm>

#include <zlib.h>

namespace lucent::zip::detail {
namespace {
class EntryOutput {
public:
  EntryOutput(const Entry &entry, const EntrySink &sink) : entry_(entry), sink_(sink) {
  }
  bool append(ByteView bytes, std::string &error) {
    if (bytes.size() > entry_.uncompressed_size - written_) {
      error = "archive entry exceeded its declared expanded size";
      return false;
    }
    written_ += bytes.size();
    crc_ = crc32(crc_, bytes.data(), static_cast<uInt>(bytes.size()));
    if (sink_ && !bytes.empty() && !sink_(bytes)) {
      error = "could not write extracted archive entry: " + entry_.name;
      return false;
    }
    return true;
  }
  bool complete(std::string &error) const {
    if (written_ != entry_.uncompressed_size) {
      error = "archive entry has inconsistent expanded size";
      return false;
    }
    if (crc_ != entry_.crc) {
      error = "archive entry failed CRC validation";
      return false;
    }
    return true;
  }

private:
  const Entry &entry_;
  const EntrySink &sink_;
  std::uint64_t written_ = 0;
  uLong crc_ = crc32(0, nullptr, 0);
};

class Inflater {
public:
  Inflater() : ready_(inflateInit2(&stream_, -MAX_WBITS) == Z_OK) {
  }
  ~Inflater() {
    if (ready_) {
      inflateEnd(&stream_);
    }
  }
  Inflater(const Inflater &) = delete;
  Inflater &operator=(const Inflater &) = delete;
  bool ready() const {
    return ready_;
  }
  z_stream &stream() {
    return stream_;
  }

private:
  z_stream stream_{};
  bool ready_;
};

bool inflate_entry(ArchiveReader &archive, std::uint64_t offset, const Entry &entry,
                   EntryOutput &output, std::string &error) {
  Inflater inflater;
  if (!inflater.ready()) {
    error = "could not initialize archive deflate decompression";
    return false;
  }
  z_stream &stream = inflater.stream();
  Bytes input(read_chunk_bytes), expanded(read_chunk_bytes);
  std::uint64_t read = 0;
  for (;;) {
    if (stream.avail_in == 0 && read < entry.compressed_size) {
      const auto count = static_cast<std::size_t>(
          std::min<std::uint64_t>(input.size(), entry.compressed_size - read));
      if (!archive.read(offset + read, std::span{input}.first(count), error)) {
        return false;
      }
      read += count;
      stream.next_in = input.data();
      stream.avail_in = static_cast<uInt>(count);
    }
    stream.next_out = expanded.data();
    stream.avail_out = static_cast<uInt>(expanded.size());
    const uLong before = stream.total_in;
    const int result = inflate(&stream, Z_NO_FLUSH);
    const std::size_t produced = expanded.size() - stream.avail_out;
    if (!output.append(std::span{expanded}.first(produced), error)) {
      return false;
    }
    if (result == Z_STREAM_END) {
      if (stream.total_in == entry.compressed_size) {
        return output.complete(error);
      }
      error = "archive entry has trailing compressed bytes";
      return false;
    }
    if (result != Z_OK || (produced == 0 && stream.total_in == before)) {
      error = "archive entry failed deflate decompression";
      return false;
    }
  }
}
} // namespace

bool stream_entry(ArchiveReader &archive, const Entry &entry, const EntrySink &sink,
                  std::string &error) {
  if ((entry.flags & 1U) != 0 || (entry.method != 0 && entry.method != 8)) {
    error = "archive entry is encrypted or uses an unsupported compression method";
    return false;
  }
  std::uint64_t offset = 0;
  if (!local_data_offset(archive, entry, offset, error)) {
    return false;
  }
  EntryOutput output(entry, sink);
  if (entry.method == 8) {
    return inflate_entry(archive, offset, entry, output, error);
  }
  if (entry.compressed_size != entry.uncompressed_size) {
    error = "stored archive entry has inconsistent sizes";
    return false;
  }
  Bytes buffer(read_chunk_bytes);
  std::uint64_t read = 0;
  while (read < entry.compressed_size) {
    const auto count = static_cast<std::size_t>(
        std::min<std::uint64_t>(buffer.size(), entry.compressed_size - read));
    const auto chunk = std::span{buffer}.first(count);
    if (!archive.read(offset + read, chunk, error) || !output.append(chunk, error)) {
      return false;
    }
    read += count;
  }
  return output.complete(error);
}

bool unpack_entry(ArchiveReader &archive, const Entry &entry, Bytes &unpacked, std::string &error) {
  unpacked.clear();
  unpacked.reserve(entry.uncompressed_size);
  return stream_entry(
      archive, entry,
      [&](ByteView chunk) {
        unpacked.insert(unpacked.end(), chunk.begin(), chunk.end());
        return true;
      },
      error);
}
} // namespace lucent::zip::detail

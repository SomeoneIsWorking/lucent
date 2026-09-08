#pragma once

#include "zip_directory.h"

namespace lucent::zip::detail {
using EntrySink = std::function<bool(ByteView)>;
// CRC and byte counts are validated even when the sink is empty (validation).
bool stream_entry(ArchiveReader &archive, const Entry &entry, const EntrySink &sink,
                  std::string &error);
// ContentMatcher's span contract requires one complete expanded entry. Only
// that API uses this adapter, after directory parsing has checked its budget.
bool unpack_entry(ArchiveReader &archive, const Entry &entry, Bytes &unpacked, std::string &error);
} // namespace lucent::zip::detail

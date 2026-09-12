#include "lucent/zip.h"
#include "zip_directory.h"
#include "zip_entry.h"
#include "zip_reader.h"

#include <algorithm>
#include <exception>
#include <fstream>
#include <system_error>
#include <utility>

namespace lucent::zip {
namespace {
using namespace detail;
struct Candidate {
  const Entry *entry = nullptr;
  bool nested = false;
};

enum class Publication : std::uint8_t { Atomic, Unpublished };

bool notify_progress(const ProgressCallback &callback, std::uint64_t done, std::uint64_t total,
                     std::string &error) {
  if (!callback) {
    return true;
  }
  try {
    callback(done, total);
    return true;
  } catch (const std::exception &exception) {
    error = "archive extraction progress callback failed: " + std::string{exception.what()};
  } catch (...) {
    error = "archive extraction progress callback failed";
  }
  return false;
}

bool write_entries(ArchiveReader &bytes, const std::vector<Entry> &archive_entries,
                   const std::filesystem::path &staging, const ProgressCallback &on_progress,
                   std::string &error) {
  std::uint64_t total = 0;
  for (const Entry &entry : archive_entries) {
    total += entry.uncompressed_size;
  }
  std::uint64_t done = 0;
  std::uint64_t interval = std::max<std::uint64_t>(1, total / 100);
  std::uint64_t next_report = interval;
  if (!notify_progress(on_progress, 0, total, error)) {
    return false;
  }
  std::error_code filesystem_error;
  for (const Entry &entry : archive_entries) {
    const std::filesystem::path output_path = staging / std::filesystem::path(entry.name);
    if (entry.name.back() == '/') {
      std::filesystem::create_directories(output_path, filesystem_error);
      if (filesystem_error) {
        error = "could not create extracted archive directory: " + output_path.string();
        return false;
      }
      continue;
    }
    std::filesystem::create_directories(output_path.parent_path(), filesystem_error);
    if (filesystem_error) {
      error = "could not create extracted archive directory: " + output_path.parent_path().string();
      return false;
    }
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
      error = "could not create extracted archive file: " + output_path.string();
      return false;
    }
    std::string progress_error;
    auto write = [&](ByteView chunk) {
      output.write(reinterpret_cast<const char *>(chunk.data()),
                   static_cast<std::streamsize>(chunk.size()));
      if (!output) {
        return false;
      }
      done += chunk.size();
      if (done >= next_report && done < total) {
        if (!notify_progress(on_progress, done, total, progress_error)) {
          return false;
        }
        next_report = done + interval;
      }
      return true;
    };
    if (!stream_entry(bytes, entry, write, error)) {
      if (!progress_error.empty()) {
        error = std::move(progress_error);
      }
      return false;
    }
    output.close();
    if (!output) {
      error = "could not finish extracted archive file: " + output_path.string();
      return false;
    }
  }
  return notify_progress(on_progress, total, total, error);
}

void discard_extraction(const std::filesystem::path &staging, std::string &error) {
  std::error_code cleanup_error;
  std::filesystem::remove_all(staging, cleanup_error);
  if (cleanup_error) {
    error += "; additionally could not remove extraction directory: " + staging.string();
  }
}

bool extract_entries(ArchiveReader &bytes, const std::vector<Entry> &archive_entries,
                     const std::filesystem::path &destination,
                     std::vector<std::filesystem::path> &files, std::string &error,
                     Publication publication, const ProgressCallback &on_progress) {
  const std::filesystem::path parent =
      destination.parent_path().empty() ? std::filesystem::path{"."} : destination.parent_path();
  std::error_code filesystem_error;
  if (!std::filesystem::is_directory(parent, filesystem_error) || filesystem_error) {
    error = "extraction destination parent does not exist: " + parent.string();
    return false;
  }
  if (std::filesystem::exists(destination, filesystem_error) || filesystem_error) {
    error = filesystem_error ? "could not inspect extraction destination"
                             : "extraction destination already exists";
    return false;
  }
  std::filesystem::path staging = destination;
  if (publication == Publication::Atomic) {
    staging += ".lucent-stage";
  }
  if (std::filesystem::exists(staging, filesystem_error) || filesystem_error) {
    error = filesystem_error ? "could not inspect extraction staging path"
                             : "extraction staging path already exists: " + staging.string();
    return false;
  }
  if (!std::filesystem::create_directory(staging, filesystem_error) || filesystem_error) {
    error = "could not create extraction staging directory: " + staging.string();
    return false;
  }
  if (!write_entries(bytes, archive_entries, staging, on_progress, error)) {
    discard_extraction(staging, error);
    return false;
  }
  if (publication == Publication::Atomic) {
    std::filesystem::rename(staging, destination, filesystem_error);
    if (filesystem_error) {
      error = "could not publish extracted archive: " + destination.string();
      discard_extraction(staging, error);
      return false;
    }
  }

  std::vector<std::filesystem::path> published;
  for (const Entry &entry : archive_entries) {
    if (entry.name.back() != '/') {
      published.push_back(destination / std::filesystem::path(entry.name));
    }
  }
  files.swap(published);
  return true;
}

bool extract_atomically(ArchiveReader &bytes, const std::vector<Entry> &archive_entries,
                        const std::filesystem::path &destination,
                        std::vector<std::filesystem::path> &files, std::string &error) {
  return extract_entries(bytes, archive_entries, destination, files, error, Publication::Atomic,
                         {});
}

bool zip_candidate(const Entry &entry, ByteView content) {
  if (equal_name(std::filesystem::path(entry.name).extension().string(), ".zip")) {
    return true;
  }
  return content.size() >= 4 && content[0] == 'P' && content[1] == 'K' &&
         ((content[2] == 3 && content[3] == 4) || (content[2] == 5 && content[3] == 6));
}

bool match_content(const ContentMatcher &matches, const Entry &entry, ByteView content,
                   Candidate &candidate, bool nested, std::string &error) {
  bool matched = false;
  try {
    matched = matches(entry.name, content);
  } catch (const std::exception &exception) {
    error = "content matcher failed for " + entry.name + ": " + exception.what();
    return false;
  }
  if (!matched) {
    return true;
  }
  if (candidate.entry != nullptr) {
    error = "more than one archive entry matched the required content identity";
    return false;
  }
  candidate = {.entry = &entry, .nested = nested};
  return true;
}

bool inspect_inner_archive(ArchiveReader &inner_bytes, const ContentMatcher &matches,
                           const ExtractionLimits &limits, Budget &budget,
                           std::vector<Entry> &inner_entries, Candidate &candidate,
                           std::string &error) {
  if (!entries(inner_bytes, inner_entries, limits, budget, error)) {
    return false;
  }
  Bytes content;
  for (const Entry &entry : inner_entries) {
    if (!unpack_entry(inner_bytes, entry, content, error)) {
      return false;
    }
    if (entry.name.back() == '/') {
      continue;
    }
    if (zip_candidate(entry, content)) {
      error = "archive contains a ZIP nested more than one level deep: " + entry.name;
      return false;
    }
    if (!match_content(matches, entry, content, candidate, true, error)) {
      return false;
    }
  }
  return true;
}

bool extract_unique_install_impl(ArchiveReader &archive, const std::filesystem::path &destination,
                                 const ContentMatcher &matches, std::filesystem::path &matched_file,
                                 std::string &error, const ExtractionLimits &limits) {
  if (!matches) {
    error = "archive content matcher is empty";
    return false;
  }
  Budget budget;
  std::vector<Entry> outer_entries;
  if (!entries(archive, outer_entries, limits, budget, error)) {
    return false;
  }

  Candidate candidate;
  Bytes content;
  Bytes inner_bytes;
  std::vector<Entry> inner_entries;
  bool found_nested_archive = false;
  for (const Entry &entry : outer_entries) {
    if (!unpack_entry(archive, entry, content, error)) {
      return false;
    }
    if (entry.name.back() == '/') {
      continue;
    }
    if (!zip_candidate(entry, content)) {
      if (!match_content(matches, entry, content, candidate, false, error)) {
        return false;
      }
      continue;
    }
    if (found_nested_archive) {
      error = "archive contains more than one nested ZIP";
      return false;
    }
    found_nested_archive = true;
    inner_bytes = content;
    SpanArchive inner_archive(inner_bytes);
    if (!inspect_inner_archive(inner_archive, matches, limits, budget, inner_entries, candidate,
                               error)) {
      error.insert(0, ": ");
      error.insert(0, entry.name);
      error.insert(0, "nested ZIP ");
      return false;
    }
  }
  if (candidate.entry == nullptr) {
    error = "no archive entry matched the required content identity";
    return false;
  }

  std::vector<std::filesystem::path> files;
  SpanArchive inner_archive(inner_bytes);
  ArchiveReader &selected_bytes =
      candidate.nested ? static_cast<ArchiveReader &>(inner_archive) : archive;
  const std::vector<Entry> &selected_entries = candidate.nested ? inner_entries : outer_entries;
  const std::string selected_name = candidate.entry->name;
  if (!extract_atomically(selected_bytes, selected_entries, destination, files, error)) {
    return false;
  }
  matched_file = destination / std::filesystem::path(selected_name);
  return true;
}

bool extract_archive_reader(ArchiveReader &archive, const std::filesystem::path &destination,
                            std::vector<std::filesystem::path> &files, std::string &error,
                            ExtractionLimits limits) {
  Budget budget;
  std::vector<Entry> archive_entries;
  std::vector<std::filesystem::path> published;
  std::string failure;
  if (!entries(archive, archive_entries, limits, budget, failure) ||
      !extract_atomically(archive, archive_entries, destination, published, failure)) {
    error = std::move(failure);
    return false;
  }
  files.swap(published);
  error.clear();
  return true;
}

} // namespace

bool extract_archive(const std::filesystem::path &archive, const std::filesystem::path &destination,
                     std::vector<std::filesystem::path> &files, std::string &error,
                     ExtractionLimits limits) {
  FileArchive reader;
  std::string failure;
  if (!reader.open(archive, limits, failure) ||
      !extract_archive_reader(reader, destination, files, failure, limits)) {
    error = std::move(failure);
    return false;
  }
  error.clear();
  return true;
}

bool extract_archive(std::span<const std::uint8_t> archive,
                     const std::filesystem::path &destination,
                     std::vector<std::filesystem::path> &files, std::string &error,
                     ExtractionLimits limits) {
  SpanArchive reader(archive);
  return extract_archive_reader(reader, destination, files, error, limits);
}

bool find_unique_file(const std::vector<std::filesystem::path> &files, const FileMatcher &matches,
                      std::filesystem::path &file, std::string &error) {
  if (!matches) {
    error = "extracted-file matcher is empty";
    return false;
  }
  std::filesystem::path selected;
  try {
    for (const std::filesystem::path &candidate : files) {
      if (!matches(candidate)) {
        continue;
      }
      if (!selected.empty()) {
        error = "more than one extracted file matched the required identity";
        return false;
      }
      selected = candidate;
    }
  } catch (const std::exception &exception) {
    error = "extracted-file matcher failed: " + std::string{exception.what()};
    return false;
  }
  if (selected.empty()) {
    error = "no extracted file matched the required identity";
    return false;
  }
  file = std::move(selected);
  error.clear();
  return true;
}

bool extract_unique_install(const std::filesystem::path &archive,
                            const std::filesystem::path &destination, const ContentMatcher &matches,
                            std::filesystem::path &matched_file, std::string &error,
                            ExtractionLimits limits) {
  FileArchive reader;
  std::string failure;
  std::filesystem::path selected = matched_file;
  if (!reader.open(archive, limits, failure) ||
      !extract_unique_install_impl(reader, destination, matches, selected, failure, limits)) {
    error = std::move(failure);
    return false;
  }
  matched_file = std::move(selected);
  error.clear();
  return true;
}

bool extract_unique_install(std::span<const std::uint8_t> archive,
                            const std::filesystem::path &destination, const ContentMatcher &matches,
                            std::filesystem::path &matched_file, std::string &error,
                            ExtractionLimits limits) {
  std::string failure;
  std::filesystem::path selected = matched_file;
  SpanArchive reader(archive);
  if (!extract_unique_install_impl(reader, destination, matches, selected, failure, limits)) {
    error = std::move(failure);
    return false;
  }
  matched_file = std::move(selected);
  error.clear();
  return true;
}

namespace {
struct InstallPaths {
  const std::filesystem::path &archive;
  const std::filesystem::path &destination;
};

bool extract_install_impl(InstallPaths paths, std::string_view required_name,
                          std::filesystem::path &executable, std::string &error,
                          ExtractionLimits limits, Publication publication,
                          const ProgressCallback &on_progress) {
  FileArchive reader;
  std::string failure;
  if (!reader.open(paths.archive, limits, failure)) {
    error = std::move(failure);
    return false;
  }
  Budget budget;
  std::vector<Entry> archive_entries;
  if (!entries(reader, archive_entries, limits, budget, failure)) {
    error = std::move(failure);
    return false;
  }
  const auto matches = std::count_if(
      archive_entries.begin(), archive_entries.end(), [required_name](const Entry &entry) {
        return entry.name.back() != '/' &&
               equal_name(std::filesystem::path(entry.name).filename().string(), required_name);
      });
  if (matches != 1) {
    error = matches == 0 ? "archive does not contain the required executable"
                         : "archive contains more than one matching executable";
    return false;
  }
  std::vector<std::filesystem::path> files;
  if (!extract_entries(reader, archive_entries, paths.destination, files, failure, publication,
                       on_progress)) {
    error = std::move(failure);
    return false;
  }
  executable = *std::find_if(files.begin(), files.end(), [required_name](const auto &file) {
    return equal_name(file.filename().string(), required_name);
  });
  error.clear();
  return true;
}
} // namespace

// Keep the existing source API while archive and destination retain distinct documented roles.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool extract_install(const std::filesystem::path &archive, const std::filesystem::path &destination,
                     std::string_view required_name, std::filesystem::path &executable,
                     std::string &error, ExtractionLimits limits) {
  return extract_install_impl({archive, destination}, required_name, executable, error, limits,
                              Publication::Atomic, {});
}

bool extract_install_unpublished(const std::filesystem::path &archive,
                                 const std::filesystem::path &destination,
                                 std::string_view required_name, std::filesystem::path &executable,
                                 std::string &error, ExtractionLimits limits,
                                 const ProgressCallback &on_progress) {
  return extract_install_impl({archive, destination}, required_name, executable, error, limits,
                              Publication::Unpublished, on_progress);
}

} // namespace lucent::zip

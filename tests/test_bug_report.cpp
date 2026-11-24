// The bug report's folder lifecycle and its two renderings, through the shipping Draft.
#include <lucent/bug_report.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

namespace {

namespace fs = std::filesystem;

int g_failures = 0;

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);                    \
      ++g_failures;                                                                                \
    }                                                                                              \
  } while (0)

fs::path make_root(const char *name) {
  const fs::path root = fs::temp_directory_path() / "lucent-bug-report-tests" / name;
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  return root;
}

void write_file(const fs::path &path, const std::string &contents) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

std::string read_file(const fs::path &path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::size_t entries_in(const fs::path &root) {
  return static_cast<std::size_t>(
      std::distance(fs::directory_iterator(root), fs::directory_iterator()));
}

std::chrono::system_clock::time_point fixed_now() {
  return std::chrono::system_clock::time_point(std::chrono::seconds(1790000000));
}

// Where `file` goes in `draft`; a refused name fails the test and yields a path nothing writes to.
fs::path attachment_path(const lucent::bug_report::Draft &draft, const char *file) {
  std::string error;
  const std::optional<fs::path> path = draft.path_for(file, error);
  CHECK(path.has_value());
  return path.value_or(fs::path());
}

std::optional<lucent::bug_report::Draft> open_draft(const fs::path &root) {
  std::string error;
  std::optional<lucent::bug_report::Draft> draft =
      lucent::bug_report::Draft::open(root, "Test Title", fixed_now(), error);
  CHECK(draft.has_value());
  CHECK(error.empty());
  return draft;
}

void commit_writes_report_and_renames() {
  const fs::path root = make_root("commit");
  std::optional<lucent::bug_report::Draft> draft = open_draft(root);
  if (!draft) {
    return;
  }
  CHECK(draft->directory().filename().string().ends_with(".draft"));

  std::string error;
  write_file(attachment_path(*draft, "screen.png"), "png");
  CHECK(
      draft->attach("screen.png", lucent::bug_report::AttachmentRole::Screenshot, "Native", error));
  draft->add_fact("frame", "1234");
  draft->set_reproduction({"Replay the pad", {"step one"}, "replay \"repro.pad\""});

  const std::optional<fs::path> committed = draft->commit({"Tomba falls | through the floor",
                                                           "line one\nline \"two\"",
                                                           {{"screen.png", 0.25, 0.1, 0.5, 0.3}}},
                                                          error);
  CHECK(committed.has_value());
  CHECK(!draft->is_open());
  if (!committed) {
    return;
  }
  CHECK(committed->filename() == "20260921-141320-tomba-falls-through-the-floor");
  CHECK(fs::is_regular_file(*committed / "screen.png"));
  CHECK(!fs::exists(*committed / ".bug-report-draft"));

  const std::string json = read_file(*committed / lucent::bug_report::kReportFileName);
  CHECK(json.find("\"format\": \"bug-report/1\"") != std::string::npos);
  CHECK(json.find("\"created\": \"2026-09-21T14:13:20Z\"") != std::string::npos);
  CHECK(json.find("\"description\": \"line one\\nline \\\"two\\\"\"") != std::string::npos);
  CHECK(json.find("\"role\": \"screenshot\"") != std::string::npos);
  CHECK(json.find("\"command\": \"replay \\\"repro.pad\\\"\"") != std::string::npos);
  CHECK(json.find("\"x\": 0.2500") != std::string::npos);
  CHECK(json.find("\"height\": 0.3000") != std::string::npos);

  const std::string readme = read_file(*committed / lucent::bug_report::kReadmeFileName);
  CHECK(readme.starts_with("# Tomba falls | through the floor\n"));
  CHECK(readme.find("![Native](screen.png)") != std::string::npos);
  CHECK(readme.find("| frame | 1234 |") != std::string::npos);
  CHECK(readme.find("- Marked: x 25%–75%, y 10%–40% of the picture") != std::string::npos);
  CHECK(entries_in(root) == 1);
}

void refusals() {
  const fs::path root = make_root("refusals");
  std::optional<lucent::bug_report::Draft> draft = open_draft(root);
  if (!draft) {
    return;
  }
  std::string error;
  CHECK(!draft->path_for("../escape.png", error).has_value());
  CHECK(!draft->path_for("sub/dir.png", error).has_value());
  CHECK(!draft->path_for("..", error).has_value());
  CHECK(!draft->path_for(lucent::bug_report::kReportFileName, error).has_value());
  CHECK(!draft->attach("missing.png", lucent::bug_report::AttachmentRole::Screenshot, "x", error));

  write_file(attachment_path(*draft, "a.pad"), "pad");
  CHECK(draft->attach("a.pad", lucent::bug_report::AttachmentRole::Reproduction, "pad", error));
  CHECK(!draft->attach("a.pad", lucent::bug_report::AttachmentRole::Reproduction, "pad", error));

  // A mark must lie on an attached picture, inside it.
  write_file(attachment_path(*draft, "shot.png"), "png");
  CHECK(draft->attach("shot.png", lucent::bug_report::AttachmentRole::Screenshot, "shot", error));
  error.clear();
  CHECK(!draft->commit({"bad mark", "", {{"a.pad", 0, 0, 0.5, 0.5}}}, error).has_value());
  CHECK(!error.empty());
  CHECK(!draft->commit({"bad mark", "", {{"shot.png", 0.8, 0, 0.5, 0.5}}}, error).has_value());
  CHECK(!draft->commit({"bad mark", "", {{"shot.png", 0, 0, 0, 0.5}}}, error).has_value());

  error.clear();
  CHECK(!draft->commit({"  \n", ""}, error).has_value());
  CHECK(!error.empty());
  CHECK(draft->is_open());
}

void discard_and_stale_pruning() {
  const fs::path root = make_root("discard");
  fs::path first;
  {
    std::optional<lucent::bug_report::Draft> draft = open_draft(root);
    if (!draft) {
      return;
    }
    first = draft->directory();
    CHECK(fs::is_directory(first));
  }
  CHECK(!fs::exists(first)); // destroyed uncommitted = discarded

  // A draft left by a process that died, and a look-alike this library did not create.
  fs::create_directories(root / "20200101-000000.draft");
  write_file(root / "20200101-000000.draft" / ".bug-report-draft", "");
  fs::create_directories(root / "keep.draft");
  lucent::bug_report::Draft::prune_abandoned(root);
  CHECK(!fs::exists(root / "20200101-000000.draft"));
  CHECK(fs::is_directory(root / "keep.draft"));

  // Opening a draft never prunes: another open draft in this process is not abandoned.
  std::optional<lucent::bug_report::Draft> draft = open_draft(root);

  // Two drafts in the same second do not share a directory.
  std::optional<lucent::bug_report::Draft> second = open_draft(root);
  if (draft && second) {
    CHECK(draft->directory() != second->directory());
  }
}

} // namespace

int main() {
  commit_writes_report_and_renames();
  refusals();
  discard_and_stale_pruning();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("lucent_bug_report_tests: all checks passed\n");
  return 0;
}

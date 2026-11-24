// lucent/bug_report.h — one bug report as a folder anyone (or any agent) can read: the player's
// words, the facts and files the application captured when the report was opened, and how to get
// back to that moment. Lucent owns the report's content, its on-disk layout and the folder's
// lifecycle; the application owns the hotkey, freezing, what it captures, the form that collects
// the words and where the reports live.
//
//   <root>/20261005-142233-tomba-falls-through-the-floor/
//     report.json   format "bug-report/1"
//     README.md     the same report for a person
//     screen.png …  what the application attached
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lucent::bug_report {

// Written into every report.json, so a reader can refuse a layout it does not understand.
inline constexpr std::string_view kFormat = "bug-report/1";
inline constexpr std::string_view kReportFileName = "report.json";
inline constexpr std::string_view kReadmeFileName = "README.md";

// What an attached file is for. Pictures are shown by a form; the other roles are listed.
enum class AttachmentRole : std::uint8_t {
  Screenshot,   // the picture the player saw
  Reference,    // a picture to compare against (an oracle or reference renderer's view)
  Reproduction, // an input recording, save or state a reproduction needs
  Context,      // anything else a reader should have (logs, dumps)
};

std::string_view role_name(AttachmentRole role);
bool is_picture(AttachmentRole role);

struct Attachment {
  std::string file; // one path component inside the report's folder
  AttachmentRole role = AttachmentRole::Context;
  std::string label; // what a reader is looking at
};

struct Fact {
  std::string name;
  std::string value;
};

// How to get back to the reported moment without the player.
struct Reproduction {
  std::string summary;            // one line: what replaying does
  std::vector<std::string> steps; // ordered instructions a person or agent follows
  std::string command;            // one command line that replays it, or empty
};

// A region the player marked on one picture, in fractions of its width and height from its top
// left, so it holds at any size the picture is shown or stored.
struct Mark {
  std::string file; // the picture attachment it is drawn on
  double x = 0;
  double y = 0;
  double width = 0;
  double height = 0;
};

// What a form collects and a commit records.
struct PlayerText {
  std::string summary; // one line, required
  std::string description;
  std::vector<Mark> marks;
};

struct Report {
  std::string application;
  std::string created; // ISO-8601 UTC, e.g. 2026-10-05T12:34:56Z
  std::string summary;
  std::string description;
  std::vector<Fact> facts;
  std::vector<Attachment> attachments;
  std::vector<Mark> marks;
  Reproduction reproduction;
};

// report.json is the machine-readable authority; README.md is the same content for a person.
std::string to_json(const Report &report);
std::string to_markdown(const Report &report);

// One report being written, and the folder that holds its files. `open` makes
// `<root>/<stamp>.draft/`; the application writes its captures to `path_for(name)` and records
// them with `attach`; `commit` writes report.json and README.md and renames the folder to
// `<root>/<stamp>-<slug>/`. A draft destroyed uncommitted removes its folder; one left by a process
// that died mid-report is removed by `prune_abandoned`, called once at startup.
class Draft {
public:
  // Makes a draft under `root` (created if missing); nothing, with `error`, on failure.
  static std::optional<Draft> open(const std::filesystem::path &root, std::string_view application,
                                   std::chrono::system_clock::time_point now, std::string &error);

  // Removes every draft folder this module made under `root`; never a committed report. Call only
  // while no draft under `root` can be open.
  static void prune_abandoned(const std::filesystem::path &root);

  Draft(Draft &&other) noexcept;
  Draft &operator=(Draft &&other) noexcept;
  Draft(const Draft &) = delete;
  Draft &operator=(const Draft &) = delete;
  ~Draft();

  // True until a commit succeeds or `discard` runs; every other accessor wants an open draft.
  bool is_open() const;
  const std::filesystem::path &directory() const;

  // Where an attachment named `file` is written. `file` must be one plain path component, not a
  // report file's name; otherwise nothing, with `error`.
  std::optional<std::filesystem::path> path_for(std::string_view file, std::string &error) const;

  // Records a file already written at path_for(file); refuses a missing file or a name twice.
  bool attach(std::string_view file, AttachmentRole role, std::string_view label,
              std::string &error);
  void add_fact(std::string_view name, std::string_view value);
  void set_reproduction(Reproduction reproduction);

  const Report &report() const;

  // Writes the report with the player's words and moves it to its final folder, whose path it
  // gives. Refuses an empty summary or a mark outside an attached picture; after a failed commit
  // the draft still owns its folder.
  std::optional<std::filesystem::path> commit(const PlayerText &text, std::string &error);

  // Removes the draft's folder and everything in it. Idempotent.
  void discard();

private:
  struct State;
  explicit Draft(std::unique_ptr<State> state);

  std::unique_ptr<State> state_;
};

} // namespace lucent::bug_report

#include <lucent/bug_report.h>

#include <lucent/text.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace lucent::bug_report {

namespace {

// Pretty-printed JSON with commas and indentation tracked by the writer, so every caller states
// only structure: a key opens a member, a value or container fills it.
class JsonWriter {
public:
  void key(std::string_view name) {
    separate();
    out_ += lucent::text::json_quote(name);
    out_ += ": ";
    pending_key_ = true;
  }
  void value(std::string_view text) {
    separate();
    out_ += lucent::text::json_quote(text);
  }
  void number(double value) {
    separate();
    char text[32];
    std::snprintf(text, sizeof text, "%.4f", value);
    out_ += text;
  }
  void begin_object() {
    open('{');
  }
  void end_object() {
    close('}');
  }
  void begin_array() {
    open('[');
  }
  void end_array() {
    close(']');
  }
  std::string finish() {
    out_ += '\n';
    return std::move(out_);
  }

private:
  // Before a member or element: a comma after a sibling, then a new line at the current depth.
  // A value directly after its key stays on the key's line.
  void separate() {
    if (pending_key_) {
      pending_key_ = false;
      return;
    }
    if (!has_sibling_.empty()) {
      if (has_sibling_.back()) {
        out_ += ',';
      }
      has_sibling_.back() = true;
      out_ += '\n';
      out_.append(has_sibling_.size() * 2, ' ');
    }
  }
  void open(char bracket) {
    separate();
    out_ += bracket;
    has_sibling_.push_back(false);
  }
  void close(char bracket) {
    const bool had_members = has_sibling_.back();
    has_sibling_.pop_back();
    if (had_members) {
      out_ += '\n';
      out_.append(has_sibling_.size() * 2, ' ');
    }
    out_ += bracket;
  }

  std::string out_;
  std::vector<bool> has_sibling_; // one entry per open container
  bool pending_key_ = false;
};

// A fenced Markdown block that cannot be closed early by the text it holds.
void append_fenced(std::string &out, std::string_view text) {
  std::string fence = "```";
  while (text.find(fence) != std::string_view::npos) {
    fence += '`';
  }
  out += fence;
  out += '\n';
  out += text;
  if (text.empty() || text.back() != '\n') {
    out += '\n';
  }
  out += fence;
  out += '\n';
}

// One Markdown table cell: a pipe would split the cell and a newline would end the row.
void append_cell(std::string &out, std::string_view text) {
  for (const char ch : text) {
    if (ch == '|') {
      out += "\\|";
    } else if (ch == '\n' || ch == '\r') {
      out += ' ';
    } else {
      out += ch;
    }
  }
}

std::string percent(double fraction) {
  return std::to_string(std::lround(fraction * 100.0)) + "%";
}

// The regions marked on one picture, as spans of its width and height.
void append_marks(std::string &out, const std::vector<Mark> &marks, std::string_view file) {
  bool any = false;
  for (const Mark &mark : marks) {
    if (mark.file != file) {
      continue;
    }
    any = true;
    out += "- Marked: x ";
    out += percent(mark.x);
    out += "–";
    out += percent(mark.x + mark.width);
    out += ", y ";
    out += percent(mark.y);
    out += "–";
    out += percent(mark.y + mark.height);
    out += " of the picture\n";
  }
  if (any) {
    out += '\n';
  }
}

} // namespace

std::string_view role_name(AttachmentRole role) {
  switch (role) {
  case AttachmentRole::Screenshot:
    return "screenshot";
  case AttachmentRole::Reference:
    return "reference";
  case AttachmentRole::Reproduction:
    return "reproduction";
  case AttachmentRole::Context:
    return "context";
  }
  return "context";
}

bool is_picture(AttachmentRole role) {
  return role == AttachmentRole::Screenshot || role == AttachmentRole::Reference;
}

std::string to_json(const Report &report) {
  JsonWriter json;
  const auto field = [&json](std::string_view name, std::string_view text) {
    json.key(name);
    json.value(text);
  };
  json.begin_object();
  field("format", kFormat);
  field("application", report.application);
  field("created", report.created);
  field("summary", report.summary);
  field("description", report.description);

  json.key("facts");
  json.begin_array();
  for (const Fact &fact : report.facts) {
    json.begin_object();
    field("name", fact.name);
    field("value", fact.value);
    json.end_object();
  }
  json.end_array();

  json.key("attachments");
  json.begin_array();
  for (const Attachment &attachment : report.attachments) {
    json.begin_object();
    field("file", attachment.file);
    field("role", role_name(attachment.role));
    field("label", attachment.label);
    json.end_object();
  }
  json.end_array();

  json.key("marks");
  json.begin_array();
  for (const Mark &mark : report.marks) {
    json.begin_object();
    field("file", mark.file);
    json.key("x");
    json.number(mark.x);
    json.key("y");
    json.number(mark.y);
    json.key("width");
    json.number(mark.width);
    json.key("height");
    json.number(mark.height);
    json.end_object();
  }
  json.end_array();

  json.key("reproduction");
  json.begin_object();
  field("summary", report.reproduction.summary);
  json.key("steps");
  json.begin_array();
  for (const std::string &step : report.reproduction.steps) {
    json.value(step);
  }
  json.end_array();
  field("command", report.reproduction.command);
  json.end_object();

  json.end_object();
  return json.finish();
}

std::string to_markdown(const Report &report) {
  std::string out = "# ";
  out += report.summary;
  out += "\n\n";
  out += report.application;
  out += " — ";
  out += report.created;
  out += "\n\n";
  if (!report.description.empty()) {
    out += report.description;
    out += "\n\n";
  }

  if (!report.attachments.empty()) {
    out += "## Attachments\n\n";
    for (const Attachment &attachment : report.attachments) {
      if (is_picture(attachment.role)) {
        out += "### ";
        out += attachment.label;
        out += "\n\n![";
        out += attachment.label;
        out += "](";
        out += attachment.file;
        out += ")\n\n";
        append_marks(out, report.marks, attachment.file);
      } else {
        out += "- `";
        out += attachment.file;
        out += "` (";
        out += role_name(attachment.role);
        out += ") ";
        out += attachment.label;
        out += '\n';
      }
    }
    if (!out.ends_with("\n\n")) {
      out += '\n';
    }
  }

  const Reproduction &reproduction = report.reproduction;
  if (!reproduction.summary.empty() || !reproduction.steps.empty() ||
      !reproduction.command.empty()) {
    out += "## Reproduction\n\n";
    if (!reproduction.summary.empty()) {
      out += reproduction.summary;
      out += "\n\n";
    }
    for (std::size_t i = 0; i < reproduction.steps.size(); ++i) {
      out += std::to_string(i + 1);
      out += ". ";
      out += reproduction.steps[i];
      out += '\n';
    }
    if (!reproduction.steps.empty()) {
      out += '\n';
    }
    if (!reproduction.command.empty()) {
      append_fenced(out, reproduction.command);
      out += '\n';
    }
  }

  if (!report.facts.empty()) {
    out += "## Facts\n\n| name | value |\n|---|---|\n";
    for (const Fact &fact : report.facts) {
      out += "| ";
      append_cell(out, fact.name);
      out += " | ";
      append_cell(out, fact.value);
      out += " |\n";
    }
  }
  return out;
}

namespace {

namespace fs = std::filesystem;

// Written into every draft directory this library creates, so pruning a stale draft can never
// remove a directory that merely has a similar name.
constexpr std::string_view kDraftMarker = ".bug-report-draft";
constexpr std::string_view kDraftSuffix = ".draft";
constexpr std::size_t kSlugLimit = 48;

std::tm utc_time(std::chrono::system_clock::time_point now) {
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &seconds);
#else
  gmtime_r(&seconds, &utc);
#endif
  return utc;
}

std::string format_time(const std::tm &utc, const char *pattern) {
  char text[32];
  const std::size_t length = std::strftime(text, sizeof text, pattern, &utc);
  return std::string(text, length);
}

// Lowercase ASCII letters and digits, runs of anything else collapsed to one '-'.
std::string slug_of(std::string_view summary) {
  std::string slug;
  bool pending_dash = false;
  for (const char ch : summary) {
    const auto byte = static_cast<unsigned char>(ch);
    if (std::isalnum(byte) != 0 && byte < 0x80) {
      if (pending_dash && !slug.empty()) {
        slug += '-';
      }
      pending_dash = false;
      slug += static_cast<char>(std::tolower(byte));
      if (slug.size() >= kSlugLimit) {
        break;
      }
    } else {
      pending_dash = true;
    }
  }
  return slug.empty() ? std::string("report") : slug;
}

// `base`, or `base-2`, `base-3`, ... — the first name under `root` that does not exist yet.
fs::path fresh_path(const fs::path &root, const std::string &base, std::string_view suffix) {
  fs::path candidate = root / (base + std::string(suffix));
  for (int n = 2; fs::exists(candidate); ++n) {
    candidate = root / (base + "-" + std::to_string(n) + std::string(suffix));
  }
  return candidate;
}

bool write_file(const fs::path &path, std::string_view content, std::string &error) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  out.close();
  if (!out) {
    error = "cannot write " + path.string();
    return false;
  }
  return true;
}

bool is_draft_directory(const fs::path &directory) {
  std::error_code ec;
  const std::string name = directory.filename().string();
  return name.size() > kDraftSuffix.size() && name.ends_with(kDraftSuffix) &&
         fs::is_regular_file(directory / kDraftMarker, ec);
}

bool is_reserved_name(std::string_view file) {
  return file == kReportFileName || file == kReadmeFileName || file == kDraftMarker;
}

// A mark names an attached picture and lies inside it (a little rounding slack at the far edges).
bool marks_a_picture(const Report &report, const Mark &mark) {
  constexpr double kSlack = 1e-3;
  bool picture = false;
  for (const Attachment &attachment : report.attachments) {
    picture = picture || (attachment.file == mark.file && is_picture(attachment.role));
  }
  return picture && mark.x >= 0 && mark.y >= 0 && mark.width > 0 && mark.height > 0 &&
         mark.x + mark.width <= 1 + kSlack && mark.y + mark.height <= 1 + kSlack;
}

} // namespace

struct Draft::State {
  fs::path directory;
  std::string stamp; // the directory's timestamp stem, reused for the committed name
  Report report;
};

void Draft::prune_abandoned(const fs::path &root) {
  std::error_code ec;
  for (const fs::directory_entry &entry : fs::directory_iterator(root, ec)) {
    if (entry.is_directory(ec) && is_draft_directory(entry.path())) {
      fs::remove_all(entry.path(), ec);
    }
  }
}

Draft::Draft(std::unique_ptr<State> state) : state_(std::move(state)) {
}

Draft::Draft(Draft &&other) noexcept = default;

Draft &Draft::operator=(Draft &&other) noexcept {
  if (this != &other) {
    discard();
    state_ = std::move(other.state_);
  }
  return *this;
}

Draft::~Draft() {
  discard();
}

std::optional<Draft> Draft::open(const fs::path &root, std::string_view application,
                                 std::chrono::system_clock::time_point now, std::string &error) {
  std::error_code ec;
  fs::create_directories(root, ec);
  if (ec) {
    error = "cannot create " + root.string() + ": " + ec.message();
    return std::nullopt;
  }

  const std::tm utc = utc_time(now);
  auto state = std::make_unique<State>();
  state->stamp = format_time(utc, "%Y%m%d-%H%M%S");
  state->directory = fresh_path(root, state->stamp, kDraftSuffix);
  fs::create_directory(state->directory, ec);
  if (ec) {
    error = "cannot create " + state->directory.string() + ": " + ec.message();
    return std::nullopt;
  }
  if (!write_file(state->directory / kDraftMarker, "", error)) {
    fs::remove_all(state->directory, ec);
    return std::nullopt;
  }
  state->report.application = std::string(application);
  state->report.created = format_time(utc, "%Y-%m-%dT%H:%M:%SZ");
  return Draft(std::move(state));
}

bool Draft::is_open() const {
  return state_ != nullptr;
}

const fs::path &Draft::directory() const {
  return state_->directory;
}

std::optional<fs::path> Draft::path_for(std::string_view file, std::string &error) const {
  const fs::path name(file);
  if (file.empty() || file == "." || file == ".." || name.has_parent_path() ||
      name.has_root_path() || file.find_first_of("/\\") != std::string_view::npos) {
    error = "attachment name must be one plain file name: '" + std::string(file) + "'";
    return std::nullopt;
  }
  if (is_reserved_name(file)) {
    error = "attachment name is reserved for the report itself: " + std::string(file);
    return std::nullopt;
  }
  return state_->directory / name;
}

bool Draft::attach(std::string_view file, AttachmentRole role, std::string_view label,
                   std::string &error) {
  const std::optional<fs::path> path = path_for(file, error);
  if (!path) {
    return false;
  }
  for (const Attachment &existing : state_->report.attachments) {
    if (existing.file == file) {
      error = "already attached: " + std::string(file);
      return false;
    }
  }
  std::error_code ec;
  if (!fs::is_regular_file(*path, ec)) {
    error = "attachment was not written: " + path->string();
    return false;
  }
  state_->report.attachments.push_back(Attachment{std::string(file), role, std::string(label)});
  return true;
}

void Draft::add_fact(std::string_view name, std::string_view value) {
  state_->report.facts.push_back(Fact{std::string(name), std::string(value)});
}

void Draft::set_reproduction(Reproduction reproduction) {
  state_->report.reproduction = std::move(reproduction);
}

const Report &Draft::report() const {
  return state_->report;
}

std::optional<fs::path> Draft::commit(const PlayerText &text, std::string &error) {
  if (!state_) {
    error = "this draft was already committed or discarded";
    return std::nullopt;
  }
  if (text.summary.find_first_not_of(" \t\r\n") == std::string::npos) {
    error = "write a one-line summary first";
    return std::nullopt;
  }
  for (const Mark &mark : text.marks) {
    if (!marks_a_picture(state_->report, mark)) {
      error = "a mark must lie inside an attached picture: " + mark.file;
      return std::nullopt;
    }
  }
  Report &report = state_->report;
  report.summary = text.summary;
  report.description = text.description;
  report.marks = text.marks;
  if (!write_file(state_->directory / kReportFileName, to_json(report), error) ||
      !write_file(state_->directory / kReadmeFileName, to_markdown(report), error)) {
    return std::nullopt;
  }

  fs::path final_path =
      fresh_path(state_->directory.parent_path(), state_->stamp + "-" + slug_of(text.summary), "");
  std::error_code ec;
  fs::rename(state_->directory, final_path, ec);
  if (ec) {
    error = "cannot move the report to " + final_path.string() + ": " + ec.message();
    return std::nullopt;
  }
  fs::remove(final_path / kDraftMarker, ec);
  state_.reset();
  return final_path;
}

void Draft::discard() {
  if (!state_) {
    return;
  }
  std::error_code ec;
  if (is_draft_directory(state_->directory)) {
    fs::remove_all(state_->directory, ec);
  }
  state_.reset();
}

} // namespace lucent::bug_report

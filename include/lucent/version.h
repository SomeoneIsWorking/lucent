#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace lucent::version {

// A released version, MAJOR.MINOR.PATCH. A leading "v" is accepted and dropped,
// because that is how a tag is written and how a title's own version reads.
struct Triple {
  int major = 0;
  int minor = 0;
  int patch = 0;
};

// Parse "1.2.3" or "v1.2.3". Rejects anything else: a missing component, a
// suffix, surrounding text, or a negative number. Callers compare only parsed
// versions, so a malformed tag can never be mistaken for the newest release.
std::optional<Triple> parse(std::string_view text);

// Negative when `left` is older, positive when newer, zero when equal. Only the
// numeric components are compared; 1.10.0 is newer than 1.9.0.
int compare(const Triple &left, const Triple &right);

// True when `candidate` is a strictly newer release than `current`. Either
// failing to parse is false: an unparsable version is not an update.
bool is_newer(std::string_view candidate, std::string_view current);

// The tag from a GitHub release document ("tag_name": "v1.2.3", at any nesting).
// Returns nothing when the document has no such field, so a changed or error
// response is a failure rather than an empty version.
std::optional<std::string> tag_from_release_json(std::string_view json);

} // namespace lucent::version

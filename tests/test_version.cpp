// Version comparison and release-tag extraction: the part of an update check
// that must be right on every platform, kept away from any transport.

#include <lucent/version.h>

#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAIL: " << message << '\n';
  ++failures;
}

void check_parses(std::string_view text, int major, int minor, int patch) {
  const auto parsed = lucent::version::parse(text);
  check(parsed.has_value(), "expected a parsed version");
  if (!parsed.has_value()) {
    return;
  }
  check(parsed->major == major && parsed->minor == minor && parsed->patch == patch,
        "parsed components");
}

void check_rejected(std::string_view text) {
  check(!lucent::version::parse(text).has_value(), "expected a rejected version");
}

void test_parsing() {
  check_parses("1.2.3", 1, 2, 3);
  check_parses("v0.3.0", 0, 3, 0);
  check_parses("V10.20.30", 10, 20, 30);
  check_parses("0.0.0", 0, 0, 0);
  /* The whole-string rule: a tag with a suffix is not this version. */
  check_rejected("1.2");
  check_rejected("1.2.3.4");
  check_rejected("1.2.3-rc1");
  check_rejected("1.2.3 ");
  check_rejected(" v1.2.3");
  check_rejected("x1.2.3");
  check_rejected("1.-2.3");
  check_rejected("1..3");
  check_rejected("");
  check_rejected("v");
  check_rejected("1.2.x");
}

void test_comparison() {
  using lucent::version::compare;
  using lucent::version::is_newer;
  using lucent::version::Triple;
  check(compare(Triple{1, 2, 3}, Triple{1, 2, 3}) == 0, "equal");
  check(compare(Triple{1, 2, 3}, Triple{1, 2, 4}) < 0, "patch older");
  check(compare(Triple{1, 3, 0}, Triple{1, 2, 9}) > 0, "minor newer");
  check(compare(Triple{2, 0, 0}, Triple{1, 99, 99}) > 0, "major newer");
  /* Numeric, not lexicographic: 1.10 is after 1.9. */
  check(compare(Triple{1, 10, 0}, Triple{1, 9, 0}) > 0, "numeric minor");

  check(is_newer("v0.4.0", "0.3.0"), "newer release");
  check(!is_newer("v0.3.0", "0.3.0"), "same release");
  check(!is_newer("v0.2.0", "0.3.0"), "older release");
  check(!is_newer("nightly", "0.3.0"), "unparsable candidate is not an update");
  check(!is_newer("v0.4.0", "not-a-version"), "unparsable current is not an update");
}

void test_release_json() {
  const std::string_view release =
      "{\n  \"url\": \"https://api.github.com/repos/x/y/releases/1\",\n"
      "  \"tag_name\": \"v1.4.0\",\n  \"name\": \"v1.4.0\"\n}\n";
  const auto tag = lucent::version::tag_from_release_json(release);
  check(tag.has_value() && *tag == "v1.4.0", "tag from a release document");

  const auto compact =
      lucent::version::tag_from_release_json(R"({"tag_name":"v2.0.0","draft":false})");
  check(compact.has_value() && *compact == "v2.0.0", "tag without spaces");

  /* An error response, a message, or a truncated body must not look like a
   * release with an empty tag. */
  check(!lucent::version::tag_from_release_json("{\"message\":\"Not Found\"}").has_value(),
        "error document has no tag");
  check(!lucent::version::tag_from_release_json("{\"tag_name\":").has_value(),
        "truncated document");
  check(!lucent::version::tag_from_release_json("").has_value(), "empty document");
  check(!lucent::version::tag_from_release_json("{\"tag_name\": null}").has_value(), "null tag");
}

} // namespace

int main() {
  test_parsing();
  test_comparison();
  test_release_json();
  if (failures != 0) {
    std::cerr << failures << " version check(s) failed\n";
    return 1;
  }
  std::cout << "lucent version tests passed\n";
  return 0;
}

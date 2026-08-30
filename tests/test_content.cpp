#include <lucent/content.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <span>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
  if (condition)
    return;
  std::cerr << "FAIL: " << message << '\n';
  ++failures;
}

lucent::content::Sha256 hash_text(std::string_view text) {
  return lucent::content::sha256(std::as_bytes(std::span(text.data(), text.size())));
}

} // namespace

int main(int argc, char **argv) {
  check(lucent::content::sha256_hex(hash_text("")) ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "empty SHA-256 vector disagrees");
  check(lucent::content::sha256_hex(hash_text("abc")) ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "abc SHA-256 vector disagrees");
  check(lucent::content::sha256_hex(
            hash_text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
        "multi-block SHA-256 vector disagrees");

  check(argc == 2, "test fixture path was not supplied");
  if (argc == 2) {
    std::string error;
    const auto digest = lucent::content::sha256_file(argv[1], error);
    check(digest.has_value(), "fixture file could not be hashed");
    if (digest) {
      check(lucent::content::sha256_hex(*digest) ==
                "a948904f2f0f479b8f8197694b30184b0d2ed1c1cd2a1ec0fb85d299a192a447",
            "streamed fixture SHA-256 disagrees");
    }
    const auto missing = lucent::content::sha256_file(
        std::filesystem::path(argv[1]).parent_path() / "missing-content-fixture", error);
    check(!missing && !error.empty(), "missing file did not return a diagnostic");
  }

  std::cout << "content: " << 5 - failures << " of 5 checks passed\n";
  return failures == 0 ? 0 : 1;
}

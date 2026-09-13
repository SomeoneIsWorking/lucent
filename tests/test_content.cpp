#include <lucent/content.h>
#include "test_environment.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <span>
#include <string_view>

#ifndef _WIN32
#include <pthread.h>
#endif

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAIL: " << message << '\n';
  ++failures;
}

lucent::content::Sha256 hash_text(std::string_view text) {
  return lucent::content::sha256(std::as_bytes(std::span(text.data(), text.size())));
}

struct SmallStackResult {
  bool hashed = false;
  bool reported_missing = false;
  std::string digest;
};

void *hash_from_small_stack(void *argument) {
  auto *result = static_cast<SmallStackResult *>(argument);
  const std::filesystem::path fixture = result->digest;
  std::string error;
  if (const auto value = lucent::content::sha256_file(fixture, error)) {
    result->hashed = true;
    result->digest = lucent::content::sha256_hex(*value);
  }
  result->reported_missing =
      !lucent::content::sha256_file(fixture.parent_path() / "missing-small-stack", error) &&
      !error.empty();
  return nullptr;
}

// Hashing runs on whatever stack its caller is on, and a browser build gives a
// thread only 64 KB of it, so the helper's own read buffer must not live there.
// This thread is given exactly that much; the old 64 KB stack buffer faulted here.
void check_hashing_on_a_small_stack(const std::filesystem::path &fixture) {
  constexpr std::size_t kStackBytes = std::size_t{64} * 1024;
  SmallStackResult result;
  result.digest = fixture.string();
#ifdef _WIN32
  const HANDLE thread = reinterpret_cast<HANDLE>(_beginthreadex(
      nullptr, static_cast<unsigned>(kStackBytes), hash_from_small_stack, &result, 0, nullptr));
  if (thread == nullptr) {
    check(false, "small-stack hashing thread could not be started");
    return;
  }
  WaitForSingleObject(thread, INFINITE);
  CloseHandle(thread);
#else
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);
  if (pthread_attr_setstacksize(&attributes, kStackBytes) != 0) {
    pthread_attr_destroy(&attributes);
    check(false, "small-stack hashing thread could not be sized");
    return;
  }
  pthread_t thread;
  if (pthread_create(&thread, &attributes, hash_from_small_stack, &result) != 0) {
    pthread_attr_destroy(&attributes);
    check(false, "small-stack hashing thread could not be started");
    return;
  }
  pthread_join(thread, nullptr);
  pthread_attr_destroy(&attributes);
#endif
  check(result.hashed &&
            result.digest == "a948904f2f0f479b8f8197694b30184b0d2ed1c1cd2a1ec0fb85d299a192a447",
        "streamed SHA-256 on a 64 KB stack disagrees");
  check(result.reported_missing, "missing file on a small stack did not return a diagnostic");
}

} // namespace

int main(int argc, char **argv) {
  return lucent::test::run_main([&] {
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
      check_hashing_on_a_small_stack(argv[1]);
    }

    std::cout << "content: " << 7 - failures << " of 7 checks passed\n";
    return failures == 0 ? 0 : 1;
  });
}

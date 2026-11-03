#include <lucent/content.h>
#include "test_environment.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <span>
#include <string_view>

#ifndef _WIN32
#include <pthread.h>
#include <unistd.h>
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

std::string sha512_text(std::string_view text) {
  return lucent::content::sha512_hex(
      lucent::content::sha512(std::as_bytes(std::span(text.data(), text.size()))));
}

// Runs body to completion on a new thread whose stack is stack_bytes, raised to the platform's
// minimum thread stack; false if no such thread could be started.
template <typename Body> bool run_on_sized_stack(std::size_t stack_bytes, Body &body) {
#ifdef _WIN32
  const auto entry = [](LPVOID argument) -> DWORD {
    (*static_cast<Body *>(argument))();
    return 0;
  };
  const HANDLE thread =
      CreateThread(nullptr, stack_bytes, entry, &body, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  if (thread == nullptr) {
    return false;
  }
  WaitForSingleObject(thread, INFINITE);
  CloseHandle(thread);
  return true;
#else
  const auto entry = [](void *argument) -> void * {
    (*static_cast<Body *>(argument))();
    return nullptr;
  };
  const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  const auto minimum = static_cast<std::size_t>(sysconf(_SC_THREAD_STACK_MIN));
  // glibc on arm64 refuses less than 128 KB; macOS wants whole pages.
  const std::size_t size = (std::max(stack_bytes, minimum) + page - 1) / page * page;
  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes) != 0) {
    return false;
  }
  pthread_t thread;
  const bool started = pthread_attr_setstacksize(&attributes, size) == 0 &&
                       pthread_create(&thread, &attributes, entry, &body) == 0;
  pthread_attr_destroy(&attributes);
  if (started) {
    pthread_join(thread, nullptr);
  }
  return started;
#endif
}

// Hashing runs on whatever stack its caller is on, and a browser build gives a
// thread only 64 KB of it, so the helper's own read buffer must not live there.
void check_hashing_on_a_small_stack(const std::filesystem::path &fixture) {
  constexpr std::size_t kBrowserStackBytes = std::size_t{64} * 1024;
  bool hashed = false;
  bool reported_missing = false;
  std::string digest;
  auto hash = [&] {
    std::string error;
    if (const auto value = lucent::content::sha256_file(fixture, error)) {
      hashed = true;
      digest = lucent::content::sha256_hex(*value);
    }
    reported_missing =
        !lucent::content::sha256_file(fixture.parent_path() / "missing-small-stack", error) &&
        !error.empty();
  };
  if (!run_on_sized_stack(kBrowserStackBytes, hash)) {
    check(false, "small-stack hashing thread could not be started");
    return;
  }
  check(hashed && digest == "a948904f2f0f479b8f8197694b30184b0d2ed1c1cd2a1ec0fb85d299a192a447",
        "streamed SHA-256 on a small stack disagrees");
  check(reported_missing, "missing file on a small stack did not return a diagnostic");
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
    check(sha512_text("") == "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                             "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e",
          "empty SHA-512 vector disagrees");
    check(sha512_text("abc") == "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                                "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
          "abc SHA-512 vector disagrees");
    check(
        sha512_text("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmno"
                    "pqklmnopqrlmnopqrsmnopqrstnopqrstu") ==
            "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
            "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909",
        "multi-block SHA-512 vector disagrees");

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

    std::cout << "content: " << 10 - failures << " of 10 checks passed\n";
    return failures == 0 ? 0 : 1;
  });
}

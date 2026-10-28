#include <lucent/text.h>
#include "test_environment.h"

#include <iostream>
#include <string>
#include <system_error>

int main() {
  return lucent::test::run_main([] {
    int failures = 0;
    if (lucent::text::ascii_lower("Game.EXE 09") != "game.exe 09") {
      std::cerr << "ASCII lowercase changed the expected letters\n";
      ++failures;
    }
    if (lucent::text::ascii_lower(std::string("\xC4Z", 2)) != std::string("\xC4z", 2)) {
      std::cerr << "ASCII lowercase changed a non-ASCII byte\n";
      ++failures;
    }
    if (!lucent::text::ascii_iequals("Game.EXE", "game.exe")) {
      std::cerr << "ASCII case-insensitive comparison missed equal names\n";
      ++failures;
    }
    if (lucent::text::ascii_iequals("game.exe", "game.exe.bak") ||
        lucent::text::ascii_iequals(std::string("\xC4", 1), std::string("\xE4", 1))) {
      std::cerr << "ASCII case-insensitive comparison accepted unequal names\n";
      ++failures;
    }
    const std::string percent = "42.5% done";
    double real = 0.0;
    const auto [end, error] =
        lucent::text::from_chars_real(percent.data(), percent.data() + percent.size(), real);
    if (error != std::errc{} || real != 42.5 || end != percent.data() + 4) {
      std::cerr << "from_chars_real misread a number followed by text\n";
      ++failures;
    }
    const std::string whole = "0.25";
    float small = 0.0f;
    const auto parsed =
        lucent::text::from_chars_real(whole.data(), whole.data() + whole.size(), small);
    if (parsed.ec != std::errc{} || small != 0.25f || parsed.ptr != whole.data() + whole.size()) {
      std::cerr << "from_chars_real misread a whole real\n";
      ++failures;
    }
    const std::string word = "x1";
    if (lucent::text::from_chars_real(word.data(), word.data() + word.size(), real).ec ==
        std::errc{}) {
      std::cerr << "from_chars_real accepted a word\n";
      ++failures;
    }
    return failures == 0 ? 0 : 1;
  });
}

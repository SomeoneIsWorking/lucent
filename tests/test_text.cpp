#include <lucent/text.h>

#include <iostream>
#include <string>

int main() {
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
  return failures == 0 ? 0 : 1;
}

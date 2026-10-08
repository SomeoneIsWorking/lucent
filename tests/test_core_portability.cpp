#include "lucent/log.h"
#include "test_environment.h"

#include <cassert>
#include <string>

int main() {
  return lucent::test::run_main([] {
    static_assert(!LUCENT_HAS_STD_FORMAT);
    std::string observed;
    lucent::set_sink([&observed](lucent::Level, std::string_view line) {
      observed = line;
    });
    lucent::log(lucent::Level::Info, "portable", "raw logging needs no std::format");
    assert(observed.ends_with("[portable] raw logging needs no std::format"));
    lucent::set_sink(nullptr);
    return 0;
  });
}

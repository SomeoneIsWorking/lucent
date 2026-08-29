#include "lucent/touch.h"

#include <iostream>
#include <vector>

namespace {
int failures = 0;

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " << #condition << "\n";           \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (false)

void capture_and_multitouch() {
  lucent::touch::Router router;
  const std::vector<lucent::touch::Zone> zones = {{1, 0, 0, 100, 100, 0}, {2, 100, 0, 200, 100, 0}};
  router.set_zones(zones);
  const std::vector<lucent::touch::Contact> began = {{11, {25, 25}, lucent::touch::Phase::began},
                                                     {12, {175, 25}, lucent::touch::Phase::began}};
  const auto began_events = router.route(began);
  CHECK(began_events.size() == 2);
  CHECK(began_events[0].zone_id == 1 && began_events[1].zone_id == 2);
  const std::vector<lucent::touch::Contact> moved = {{11, {175, 25}, lucent::touch::Phase::moved}};
  const auto moved_events = router.route(moved);
  CHECK(moved_events.size() == 1 && moved_events[0].zone_id == 1);
  const std::vector<lucent::touch::Contact> ended = {{11, {175, 25}, lucent::touch::Phase::ended},
                                                     {12, {175, 25}, lucent::touch::Phase::ended}};
  CHECK(router.route(ended).size() == 2);
  CHECK(router.route(moved).empty());
}

void priority_and_cancel() {
  lucent::touch::Router router;
  const std::vector<lucent::touch::Zone> zones = {{1, 0, 0, 100, 100, 1}, {2, 0, 0, 100, 100, 2}};
  router.set_zones(zones);
  const std::vector<lucent::touch::Contact> began = {{7, {50, 50}, lucent::touch::Phase::began},
                                                     {8, {500, 500}, lucent::touch::Phase::began}};
  const auto events = router.route(began);
  CHECK(events.size() == 1 && events[0].zone_id == 2);
  const auto canceled = router.cancel();
  CHECK(canceled.size() == 1 && canceled[0].contact_id == 7);
  CHECK(router.cancel().empty());
}
} // namespace

int main() {
  capture_and_multitouch();
  priority_and_cancel();
  if (failures != 0)
    return 1;
  std::cout << "touch router: all checks passed\n";
  return 0;
}

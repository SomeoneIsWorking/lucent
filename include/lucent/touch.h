#pragma once

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace lucent::touch {

struct Point {
  float x = 0.0F;
  float y = 0.0F;
};

enum class Phase { began, moved, ended, canceled };

struct Contact {
  std::int64_t id = 0;
  Point position;
  Phase phase = Phase::moved;
};

struct Zone {
  std::uint32_t id = 0;
  float left = 0.0F;
  float top = 0.0F;
  float right = 0.0F;
  float bottom = 0.0F;
  int priority = 0;
};

struct Event {
  std::int64_t contact_id = 0;
  std::uint32_t zone_id = 0;
  Point position;
  Phase phase = Phase::moved;
};

// Maps platform touch contacts to application-owned control zones. A contact captures its zone
// on began, so moving a finger outside the visible button does not produce a second action. The
// application remains responsible for choosing zones, converting zone IDs to actions, and applying
// safe-area/inset policy.
class Router {
public:
  void set_zones(std::span<const Zone> zones);
  std::vector<Event> route(std::span<const Contact> contacts);
  std::vector<Event> cancel();

private:
  struct Capture {
    std::uint32_t zone_id = 0;
    Point position;
  };

  std::vector<Zone> zones_;
  std::vector<std::pair<std::int64_t, Capture>> captures_;
};

} // namespace lucent::touch

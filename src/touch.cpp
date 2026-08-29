#include "lucent/touch.h"

#include <algorithm>

namespace lucent::touch {
namespace {

bool contains(const Zone &zone, Point point) {
  return zone.id != 0 && zone.left <= point.x && point.x < zone.right && zone.top <= point.y &&
         point.y < zone.bottom;
}

} // namespace

void Router::set_zones(std::span<const Zone> zones) {
  zones_.assign(zones.begin(), zones.end());
  std::stable_sort(zones_.begin(), zones_.end(), [](const Zone &left, const Zone &right) {
    return left.priority > right.priority;
  });
}

std::vector<Event> Router::route(std::span<const Contact> contacts) {
  std::vector<Event> events;
  events.reserve(contacts.size());
  for (const Contact &contact : contacts) {
    auto capture = std::find_if(captures_.begin(), captures_.end(),
                                [&](const auto &entry) { return entry.first == contact.id; });
    if (contact.phase == Phase::began) {
      if (capture != captures_.end())
        captures_.erase(capture);
      const auto zone = std::find_if(zones_.begin(), zones_.end(), [&](const Zone &candidate) {
        return contains(candidate, contact.position);
      });
      if (zone == zones_.end())
        continue;
      captures_.emplace_back(contact.id, Capture{zone->id, contact.position});
      events.push_back(Event{contact.id, zone->id, contact.position, Phase::began});
      continue;
    }
    if (capture == captures_.end())
      continue;
    capture->second.position = contact.position;
    events.push_back(Event{contact.id, capture->second.zone_id, contact.position, contact.phase});
    if (contact.phase == Phase::ended || contact.phase == Phase::canceled)
      captures_.erase(capture);
  }
  return events;
}

std::vector<Event> Router::cancel() {
  std::vector<Event> events;
  events.reserve(captures_.size());
  for (const auto &entry : captures_)
    events.push_back(
        Event{entry.first, entry.second.zone_id, entry.second.position, Phase::canceled});
  captures_.clear();
  return events;
}

} // namespace lucent::touch

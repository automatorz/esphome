#pragma once

// SunScheduleHandler — small helper driven by two native sun::SunTriggers
// synthesized in mcp23017_shared_int's codegen. It turns the component's own
// fabricated `role: light` entities on/off at sunrise/sunset and, when wired,
// fires the sibling shutter_hub's schedule_* methods at the SAME configured
// elevation.
//
// It is ALSO a PollingComponent so it can publish two always-on text_sensors
// that report the wall-clock time of day (HH:MM:SS, 24h) of the NEXT sunset and
// NEXT sunrise, computed at the SAME elevation that drives the triggers. The
// recompute is pure astronomy + clock math (no I2C / hardware reads), run on a
// cheap 60s cadence; values are deduplicated so unchanged times aren't
// re-published. Because it is now a PollingComponent, it IS counted toward
// ESPHOME_COMPONENT_COUNT (see _fabricated_sun_schedule_ids in __init__.py).

#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/components/light/light_state.h"
#include "esphome/components/sun/sun.h"
#include "esphome/components/text_sensor/text_sensor.h"

// Forward-declare the shutter hub to avoid pulling shutter_hub.h into a widely
// included header. The pointer is stored opaquely and the real type is only
// needed in the .cpp (which includes shutter_hub.h).
namespace esphome {
namespace shutter_hub {
class ShutterHub;
}  // namespace shutter_hub

namespace mcp23017_shared_int {

class SunScheduleHandler : public PollingComponent {
 public:
  // Recompute cadence: time-of-day changes slowly, so a 60s arithmetic
  // recompute is plenty. This is NOT hardware polling (no I2C) — just a clock
  // read plus astronomy math.
  SunScheduleHandler() : PollingComponent(60000) {}

  struct Entry {
    light::LightState *light;
    bool turn_on;
  };

  void add_sunset_action(light::LightState *light, bool turn_on) {
    this->sunset_.push_back(Entry{light, turn_on});
  }
  void add_sunrise_action(light::LightState *light, bool turn_on) {
    this->sunrise_.push_back(Entry{light, turn_on});
  }
  void set_shutter_hub(shutter_hub::ShutterHub *hub) { this->hub_ = hub; }

  void set_sun(sun::Sun *s) { this->sun_ = s; }
  void set_elevation(double elevation) { this->elevation_ = elevation; }
  void set_next_sunset_sensor(text_sensor::TextSensor *s) { this->next_sunset_sensor_ = s; }
  void set_next_sunrise_sensor(text_sensor::TextSensor *s) { this->next_sunrise_sensor_ = s; }

  // Run slightly after default so the sun/time components are up first.
  float get_setup_priority() const override { return setup_priority::LATE; }
  void update() override;

  // Invoked from the sunset SunTrigger's lambda action.
  void on_sunset();
  // Invoked from the sunrise SunTrigger's lambda action.
  void on_sunrise();

 protected:
  // Format the next event (ascending=sunrise / descending=sunset through
  // elevation_) as HH:MM:SS and publish to sensor if it changed. Publishes the
  // placeholder "--:--:--" until the clock is synced.
  void publish_next_(bool rising, text_sensor::TextSensor *sensor, std::string *last);

  std::vector<Entry> sunset_;
  std::vector<Entry> sunrise_;
  shutter_hub::ShutterHub *hub_{nullptr};

  sun::Sun *sun_{nullptr};
  double elevation_{0.0};
  text_sensor::TextSensor *next_sunset_sensor_{nullptr};
  text_sensor::TextSensor *next_sunrise_sensor_{nullptr};
  std::string last_sunset_;
  std::string last_sunrise_;
};

}  // namespace mcp23017_shared_int
}  // namespace esphome

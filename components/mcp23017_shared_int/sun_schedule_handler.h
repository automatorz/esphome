#pragma once

// SunScheduleHandler — tiny, non-Component helper driven by two native
// sun::SunTriggers synthesized in mcp23017_shared_int's codegen. It turns the
// component's own fabricated `role: light` entities on/off at sunrise/sunset
// and, when wired, fires the sibling shutter_hub's schedule_* methods at the
// SAME configured elevation.
//
// This is deliberately NOT a Component: it holds no state machine and runs no
// setup()/loop(). It is a plain cg.new_Pvariable object whose methods are
// invoked from the SunTriggers' lambda actions, so it does NOT count toward
// ESPHOME_COMPONENT_COUNT.

#include <vector>

#include "esphome/components/light/light_state.h"

// Forward-declare the shutter hub to avoid pulling shutter_hub.h into a widely
// included header. The pointer is stored opaquely and the real type is only
// needed in the .cpp (which includes shutter_hub.h).
namespace esphome {
namespace shutter_hub {
class ShutterHub;
}  // namespace shutter_hub

namespace mcp23017_shared_int {

class SunScheduleHandler {
 public:
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

  // Invoked from the sunset SunTrigger's lambda action.
  void on_sunset();
  // Invoked from the sunrise SunTrigger's lambda action.
  void on_sunrise();

 protected:
  std::vector<Entry> sunset_;
  std::vector<Entry> sunrise_;
  shutter_hub::ShutterHub *hub_{nullptr};
};

}  // namespace mcp23017_shared_int
}  // namespace esphome

#pragma once

#include <functional>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/light/light_state.h"
#include "int_coordinator.h"
#include "mcp23017_shared_int.h"

namespace esphome {
namespace mcp23017_shared_int {

// A fully passive binary sensor. It has NO loop() and NO update(): it never reads
// the chip and never touches I2C. The coordinator pushes state via on_pin_state()
// when the shared interrupt fires and this pin's level changed.
//
// `inverted` is handled here in software so the hardware IPOL stays 0.
class MCP23017BinarySensor : public binary_sensor::BinarySensor,
                             public Component,
                             public InputPinListener {
 public:
  void set_chip(MCP23017Chip *chip) { this->chip_ = chip; }
  void set_pin(uint8_t pin) { this->pin_ = pin; }
  void set_inverted(bool inverted) {
    this->inverted_ = inverted;
    // Seed the previous logical state to the inactive (unpressed) value so the
    // first real press is detected as an OFF->ON edge. After inversion the
    // logical state is `level != inverted_`; at rest level matches inverted_,
    // yielding logical false.
    this->last_logical_ = false;
  }
  void set_coordinator(SharedIntCoordinator *coord) { this->coordinator_ = coord; }

  // Wire this input (as a momentary button) to one or more lights. On the
  // OFF->ON (press) edge of this sensor's logical state, every wired light is
  // turned on / off per its recorded intent. A single input may drive several
  // lights in any mix of on/off (e.g. one wall button turns one light ON and
  // another OFF), so these accumulate into a list rather than overwrite. Fully
  // push-driven: fires from on_pin_state(), which the coordinator calls on an
  // interrupt — no polling.
  void set_light_on(light::LightState *light) {
    this->light_actions_.push_back(LightAction{light, true});
  }
  void set_light_off(light::LightState *light) {
    this->light_actions_.push_back(LightAction{light, false});
  }

  // Generic press callback. Fired on the OFF->ON (press) edge, AFTER the
  // light_actions_ loop. Kept SEPARATE from the light-specific light_actions_
  // mechanism so other consumers (e.g. ha_light_hub) can hook the press edge
  // without disturbing the button->light wiring. Un-debounced here on purpose:
  // the raw edge stays generic; debounce lives in the consumer. The dependency
  // stays one-directional (consumer -> this sensor), so this header never
  // includes or forward-declares the consumer type.
  void add_on_press(std::function<void()> &&cb) {
    this->on_press_.push_back(std::move(cb));
  }

  void setup() override {
    // Nothing to do here: the chip input mask and the coordinator listener
    // registration are both wired at CODEGEN time in __init__.py::_gen_input_pin,
    // so no ordering-dependent work remains. The class still needs to be a
    // Component (see get_setup_priority).
  }

  void dump_config() override;

  float get_setup_priority() const override { return setup_priority::DATA; }

  // InputPinListener
  void on_pin_state(bool level) override;
  MCP23017Chip *get_chip() const override { return this->chip_; }
  uint8_t get_pin() const override { return this->pin_; }

 protected:
  MCP23017Chip *chip_{nullptr};
  SharedIntCoordinator *coordinator_{nullptr};
  uint8_t pin_{0};
  bool inverted_{false};
  // Button->light wiring (optional). Empty when this input is not wired to any
  // light, in which case on_pin_state() only publishes state. One button may
  // drive multiple lights (any mix of on/off).
  struct LightAction {
    light::LightState *light;
    bool turns_on;
  };
  std::vector<LightAction> light_actions_;
  // Generic press-edge callbacks (see add_on_press). Separate from light_actions_.
  std::vector<std::function<void()>> on_press_;
  // Previous logical (post-inversion) state, used to detect the press edge.
  bool last_logical_{false};
};

}  // namespace mcp23017_shared_int
}  // namespace esphome

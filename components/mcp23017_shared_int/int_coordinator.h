#pragma once

#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "mcp23017_shared_int.h"

namespace esphome {
namespace mcp23017_shared_int {

// Implemented by the passive binary sensor. The coordinator calls on_pin_state()
// with the freshly-read level for the sensor's (chip, pin). Sensors never read.
class InputPinListener {
 public:
  // chip index into the coordinator's chip list, pin 0..15, raw hardware level.
  virtual void on_pin_state(bool level) = 0;
  virtual MCP23017Chip *get_chip() const = 0;
  virtual uint8_t get_pin() const = 0;
};

// Owns the single shared interrupt pin (e.g. GPIO14). On a falling edge it wakes
// its loop; the loop reads every chip once, pushes changed input levels to the
// registered listeners, and keeps looping until the wired-OR line is released
// (reads HIGH), then disables itself. No polling: the loop is otherwise dormant.
class SharedIntCoordinator : public Component {
 public:
  float get_setup_priority() const override { return setup_priority::IO - 1.0f; }
  void setup() override;
  void dump_config() override;
  void loop() override;

  void set_interrupt_pin(InternalGPIOPin *pin) { this->interrupt_pin_ = pin; }
  void register_chip(MCP23017Chip *chip) { this->chips_.push_back(chip); }
  void register_listener(InputPinListener *listener) { this->listeners_.push_back(listener); }

 protected:
  static void IRAM_ATTR gpio_intr(SharedIntCoordinator *arg) { arg->enable_loop_soon_any_context(); }

  // Read all chips once and dispatch to listeners. Returns true if any chip read
  // failed (so we can decide whether to keep retrying).
  void service_once_();

  InternalGPIOPin *interrupt_pin_{nullptr};
  std::vector<MCP23017Chip *> chips_;
  std::vector<InputPinListener *> listeners_;

  // Last published level per listener, so we only publish on change.
  // Parallel to listeners_. Initialized in setup().
  std::vector<bool> last_level_;

  // On the first loop pass after setup, publish every listener's state once
  // regardless of the change check, to seed initial values.
  bool force_initial_publish_{false};

  // Safety bound on drain iterations within a single loop() to avoid a hang if
  // the INT line is stuck low due to a hardware fault.
  static constexpr uint8_t MAX_DRAIN_ITERATIONS = 8;
};

}  // namespace mcp23017_shared_int
}  // namespace esphome

#pragma once

#include "esphome/core/component.h"
#include "esphome/components/switch/switch.h"
#include "mcp23017_shared_int.h"

namespace esphome {
namespace mcp23017_shared_int {

// A switch bound to an MCP23017 OUTPUT pin. Writes go straight to OLAT via the
// hub; no interrupt involvement and no polling. `inverted` handled in software.
class MCP23017Switch : public switch_::Switch, public Component {
 public:
  void set_chip(MCP23017Chip *chip) { this->chip_ = chip; }
  void set_pin(uint8_t pin) { this->pin_ = pin; }

  void setup() override {
    // Mark the pin as an output on the chip so it is excluded from the input
    // mask / interrupt handling, then apply the restore-mode initial state.
    this->chip_->set_pin_output(this->pin_);
    auto initial = this->get_initial_state_with_restore_mode();
    if (initial.has_value())
      this->write_state(initial.value());
  }

  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  // Inversion is already applied by the base class before write_state() is
  // called, so `state` here is the raw hardware level to drive. We write it and
  // then acknowledge via publish_state() (which re-applies inversion for the
  // reported/front-end value, per the switch base contract).
  void write_state(bool state) override {
    this->chip_->write_output(this->pin_, state);
    this->publish_state(state);
  }

  MCP23017Chip *chip_{nullptr};
  uint8_t pin_{0};
};

}  // namespace mcp23017_shared_int
}  // namespace esphome

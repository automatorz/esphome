#pragma once

#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
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
  void set_inverted(bool inverted) { this->inverted_ = inverted; }
  void set_coordinator(SharedIntCoordinator *coord) { this->coordinator_ = coord; }

  void setup() override {
    // Nothing to do here: the chip input mask and the coordinator listener
    // registration are both wired at CODEGEN time in __init__.py::_gen_input_pin,
    // so no ordering-dependent work remains. The class still needs to be a
    // Component (see get_setup_priority).
  }

  void dump_config() override;

  float get_setup_priority() const override { return setup_priority::DATA; }

  // InputPinListener
  void on_pin_state(bool level) override { this->publish_state(level != this->inverted_); }
  MCP23017Chip *get_chip() const override { return this->chip_; }
  uint8_t get_pin() const override { return this->pin_; }

 protected:
  MCP23017Chip *chip_{nullptr};
  SharedIntCoordinator *coordinator_{nullptr};
  uint8_t pin_{0};
  bool inverted_{false};
};

}  // namespace mcp23017_shared_int
}  // namespace esphome

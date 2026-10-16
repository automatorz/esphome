#pragma once

#include "esphome/core/component.h"
#include "esphome/components/output/binary_output.h"
#include "mcp23017_shared_int.h"

namespace esphome {
namespace mcp23017_shared_int {

// A binary output bound to an MCP23017 OUTPUT pin. Writes go straight to OLAT
// via the hub, exactly like MCP23017Switch, but exposes the output::BinaryOutput
// interface so a stock `light: platform: binary` can drive it. Inversion is
// handled by the output base class (set_inverted) before write_state_ runs.
class MCP23017Output : public output::BinaryOutput, public Component {
 public:
  void set_chip(MCP23017Chip *chip) { this->chip_ = chip; }
  void set_pin(uint8_t pin) { this->pin_ = pin; }

  void setup() override {
    // Mark the pin as an output on the chip so it is excluded from the input
    // mask / interrupt handling. Drive it to its OFF level initially so the
    // light does not flash on at boot. For an inverted output, off = hardware
    // HIGH, so we must write the raw level = inverted_ (NOT a hard-coded false,
    // which for an inverted light is hardware-low = ON and caused a visible
    // ~1-2 s flash before the light component's restore wrote the real state).
    // This matches the chip's OLAT pre-load in configure_hardware(), so there
    // is no glitch. The light component applies its own restore mode on top.
    this->chip_->set_pin_output(this->pin_);
    this->chip_->write_output(this->pin_, this->inverted_);
  }

  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  // `state` is the raw hardware level to drive (inversion already applied by
  // the BinaryOutput base class).
  void write_state(bool state) override {
    this->chip_->write_output(this->pin_, state);
  }

  MCP23017Chip *chip_{nullptr};
  uint8_t pin_{0};
};

}  // namespace mcp23017_shared_int
}  // namespace esphome

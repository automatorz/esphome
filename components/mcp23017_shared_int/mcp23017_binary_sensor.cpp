#include "mcp23017_binary_sensor.h"
#include "esphome/core/log.h"

namespace esphome {
namespace mcp23017_shared_int {

static const char *const TAG = "mcp23017_shared_int.binary_sensor";

void MCP23017BinarySensor::on_pin_state(bool level) {
  const bool logical = level != this->inverted_;
  this->publish_state(logical);

  // Momentary-button -> light wiring. Act only on the OFF->ON (press) edge, not
  // on release, and not repeatedly while held. If no light is wired, this is a
  // no-op beyond tracking the edge.
  if (this->light_ != nullptr && logical && !this->last_logical_) {
    if (this->light_turns_on_) {
      this->light_->turn_on().perform();
    } else {
      this->light_->turn_off().perform();
    }
  }
  this->last_logical_ = logical;
}

void MCP23017BinarySensor::dump_config() {
  LOG_BINARY_SENSOR("", "MCP23017 Shared-Int Binary Sensor", this);
  ESP_LOGCONFIG(TAG, "  Chip address: 0x%02X", this->chip_->get_i2c_address());
  ESP_LOGCONFIG(TAG, "  Pin: %u", this->pin_);
  ESP_LOGCONFIG(TAG, "  Inverted: %s", YESNO(this->inverted_));
}

}  // namespace mcp23017_shared_int
}  // namespace esphome

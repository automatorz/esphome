#include "mcp23017_binary_sensor.h"
#include "esphome/core/log.h"

namespace esphome {
namespace mcp23017_shared_int {

static const char *const TAG = "mcp23017_shared_int.binary_sensor";

void MCP23017BinarySensor::dump_config() {
  LOG_BINARY_SENSOR("", "MCP23017 Shared-Int Binary Sensor", this);
  ESP_LOGCONFIG(TAG, "  Chip address: 0x%02X", this->chip_->get_i2c_address());
  ESP_LOGCONFIG(TAG, "  Pin: %u", this->pin_);
  ESP_LOGCONFIG(TAG, "  Inverted: %s", YESNO(this->inverted_));
}

}  // namespace mcp23017_shared_int
}  // namespace esphome

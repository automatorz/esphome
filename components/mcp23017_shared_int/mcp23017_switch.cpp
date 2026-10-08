#include "mcp23017_switch.h"
#include "esphome/core/log.h"

namespace esphome {
namespace mcp23017_shared_int {

static const char *const TAG = "mcp23017_shared_int.switch";

void MCP23017Switch::dump_config() {
  LOG_SWITCH("", "MCP23017 Shared-Int Switch", this);
  ESP_LOGCONFIG(TAG, "  Chip address: 0x%02X", this->chip_->get_i2c_address());
  ESP_LOGCONFIG(TAG, "  Pin: %u", this->pin_);
}

}  // namespace mcp23017_shared_int
}  // namespace esphome

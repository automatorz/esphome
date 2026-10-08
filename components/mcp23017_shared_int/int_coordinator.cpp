#include "int_coordinator.h"
#include "esphome/core/log.h"

namespace esphome {
namespace mcp23017_shared_int {

static const char *const TAG = "mcp23017_shared_int.coord";

void SharedIntCoordinator::setup() {
  ESP_LOGCONFIG(TAG, "Setting up shared-interrupt coordinator ...");

  // NOTE on ordering: listeners (binary sensors) register themselves from their
  // own setup(). To guarantee every listener has registered before we seed
  // initial state, we do NOT seed here. Instead we enable the loop once so the
  // first loop() iteration (which runs after all setup() calls complete) seeds
  // initial state and dispatches it. force_initial_publish_ makes that first
  // pass publish regardless of the "changed" check.

  // Set up the single shared interrupt pin. External pull-up assumed; the line
  // is active-low (open-drain wired-OR), so we trigger on the falling edge.
  if (this->interrupt_pin_ != nullptr) {
    this->interrupt_pin_->setup();
    this->interrupt_pin_->attach_interrupt(&SharedIntCoordinator::gpio_intr, this,
                                           gpio::INTERRUPT_FALLING_EDGE);
  } else {
    ESP_LOGE(TAG, "No interrupt pin configured; coordinator cannot run interrupt-driven.");
    this->mark_failed();
    return;
  }

  // Size the per-listener state cache now that all listeners have registered.
  this->last_level_.assign(this->listeners_.size(), false);

  // Run the loop once on the next tick to seed and publish initial state, then
  // it will go dormant until the ISR fires. We do this via the loop (not here)
  // so all listener setup() calls have completed first.
  this->force_initial_publish_ = true;
  this->enable_loop();
}

void SharedIntCoordinator::loop() {
  // Woken by the ISR. Drain the wired-OR line: read all chips, dispatch, and
  // repeat while INT remains low (another chip still asserting, or a new edge
  // arrived during the read). Bounded to avoid hanging on a stuck line.
  uint8_t iterations = 0;
  do {
    this->service_once_();
    // First pass published initial state unconditionally; clear the flag so
    // subsequent passes only publish on change.
    this->force_initial_publish_ = false;
    iterations++;
    if (iterations >= MAX_DRAIN_ITERATIONS) {
      ESP_LOGW(TAG, "INT still low after %u drain iterations; deferring to next loop.",
               iterations);
      // Leave loop enabled so we try again next tick rather than spinning here.
      return;
    }
  } while (this->interrupt_pin_->digital_read() == false);

  // INT released (HIGH): nothing more pending. Go dormant until the next edge.
  this->disable_loop();
}

void SharedIntCoordinator::service_once_() {
  // Read each chip once (one 16-bit GPIO read per chip, which also clears that
  // chip's latch), then push changed levels to listeners belonging to it.
  for (auto *chip : this->chips_) {
    uint16_t port;
    if (!chip->read_inputs(&port)) {
      // Communication error on this chip; skip, warning already set by hub.
      continue;
    }
    const uint16_t input_mask = chip->get_input_mask();
    for (size_t i = 0; i < this->listeners_.size(); i++) {
      InputPinListener *l = this->listeners_[i];
      if (l->get_chip() != chip)
        continue;
      const uint8_t pin = l->get_pin();
      // Ignore any listener accidentally bound to an output pin.
      if ((input_mask & (uint16_t(1) << pin)) == 0)
        continue;
      const bool level = (port & (uint16_t(1) << pin)) != 0;
      if (this->force_initial_publish_ || level != this->last_level_[i]) {
        this->last_level_[i] = level;
        l->on_pin_state(level);
      }
    }
  }
}

void SharedIntCoordinator::dump_config() {
  ESP_LOGCONFIG(TAG, "MCP23017 Shared-Interrupt Coordinator:");
  LOG_PIN("  Interrupt Pin: ", this->interrupt_pin_);
  ESP_LOGCONFIG(TAG, "  Chips: %u", (unsigned) this->chips_.size());
  ESP_LOGCONFIG(TAG, "  Input listeners: %u", (unsigned) this->listeners_.size());
}

}  // namespace mcp23017_shared_int
}  // namespace esphome

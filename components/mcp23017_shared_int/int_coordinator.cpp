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

  // TEMP DIAGNOSTIC: poll once a second, independent of the interrupt path, so a
  // dead INT line still produces data. Remove this (and diag_poll_ / isr_count_)
  // once wall-switch inputs are confirmed working.
  this->set_interval("mcp_diag", 1000, [this]() { this->diag_poll_(); });
}

void SharedIntCoordinator::diag_poll_() {
  const int level = (this->interrupt_pin_ != nullptr)
                        ? (this->interrupt_pin_->digital_read() ? 1 : 0)
                        : -1;
  ESP_LOGD(TAG, "DIAG: INT pin level=%d  isr_count=%u  chips=%u", level,
           (unsigned) this->isr_count_, (unsigned) this->chips_.size());
  size_t idx = 0;
  for (auto *chip : this->chips_) {
    const uint16_t mask = chip->get_input_mask();
    // Read INTF FIRST (non-clearing): tells us which input pins have a pending
    // interrupt flag latched since the last clear. A momentary press that
    // happened between polls will show here even though the pin is back to idle.
    uint16_t intf = 0;
    const bool ok_intf = chip->read_intf(&intf);
    // Then read INTCAP: the value captured at the moment of the interrupt.
    // Reading INTCAP clears the interrupt condition (like reading GPIO).
    uint16_t intcap = 0;
    const bool ok_cap = chip->read_intcap(&intcap);
    // Live GPIO for reference.
    uint16_t port = 0;
    const bool ok_gpio = chip->read_inputs(&port);
    ESP_LOGD(TAG,
             "DIAG: chip[%u] 0x%02X intf_ok=%s INTF=0x%04X intcap_ok=%s "
             "INTCAP=0x%04X gpio_ok=%s GPIO=0x%04X inputs=0x%04X",
             (unsigned) idx, chip->get_i2c_address(), ok_intf ? "y" : "N", intf,
             ok_cap ? "y" : "N", intcap, ok_gpio ? "y" : "N", port,
             (uint16_t) (port & mask));
    if (intf != 0)
      ESP_LOGW(TAG, "DIAG: chip[%u] 0x%02X LATCHED a change! INTF=0x%04X -> this "
                    "chip IS detecting presses (interrupt simply isn't reaching "
                    "the ESP).",
               (unsigned) idx, chip->get_i2c_address(), intf);
    idx++;
  }
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
  // Defensive sizing: last_level_ is indexed in parallel with listeners_. It is
  // seeded in setup(), but a listener may register in its own setup() AFTER the
  // coordinator's setup() ran (setup-priority ordering is not guaranteed to put
  // every listener first). If that happens, listeners_ is larger than
  // last_level_ and the indexed write below would run off the end of the
  // vector<bool> and crash (LoadProhibited). Grow last_level_ to match before
  // use so it always covers every registered listener.
  if (this->last_level_.size() < this->listeners_.size()) {
    this->last_level_.resize(this->listeners_.size(), false);
  }

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

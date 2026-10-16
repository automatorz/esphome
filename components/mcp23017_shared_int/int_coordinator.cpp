#include "int_coordinator.h"
#include "esphome/core/log.h"

namespace esphome {
namespace mcp23017_shared_int {

static const char *const TAG = "mcp23017_shared_int.coord";

// TEMP diagnostic state for the bounded INT-WATCH window. File-scope so the
// probe is self-contained in this .cpp (no .h dependency). Removed with probe.
static bool g_int_watch_saw_low = false;

void SharedIntCoordinator::setup() {
  ESP_LOGCONFIG(TAG, "Setting up shared-interrupt coordinator ...");

  // One-time boot write burst: program every chip's config registers now, from
  // a code path that reliably runs. A chip's own setup() may be silently
  // skipped (ESPHome's fixed component array can be sized too small and drop
  // chips registered past its capacity), leaving their interrupt registers
  // unconfigured. The coordinator holds all chips in chips_ regardless, so we
  // program them here. This must happen BEFORE the interrupt is attached and
  // BEFORE enable_loop(), so no chip can assert INT before it is configured.
  // This is NOT polling: it runs exactly once at boot.
  for (auto *chip : this->chips_) {
    chip->configure_hardware();
  }

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

  // TEMP: bounded GPIO14 watcher (self-terminating; NOT steady-state polling and
  // NO I2C). Starting 12 s after boot, sample ONLY the ESP's own interrupt GPIO
  // pin every 20 ms for 30 s, then STOP permanently. We never touch the I2C bus
  // here, so there is zero i2c_master risk. Purpose: with all 8 chips confirmed
  // configured (GPINTEN=0xFF, IOCON=0x44), the only remaining question is whether
  // the chip INT assertion actually reaches GPIO14 through the ISO7221. Press
  // wall switches during this 30 s window; if the line ever dips to 0 we will see
  // it, and the ISR count will climb. If it stays 1 the whole time, the interrupt
  // signal is not getting through the isolator (a hardware wiring issue).
  // State is a file-scope static (not a class member) so this diagnostic is
  // fully self-contained in the .cpp and does not depend on any .h change
  // (avoids header/source desync when the build tree copies files inconsistently).
  this->set_timeout("mcp_int_watch_start", 12000, [this]() {
    ESP_LOGW(TAG, "INT-WATCH: starting 30s GPIO14 watch now - PRESS WALL SWITCHES");
    this->set_interval("mcp_int_watch", 20, [this]() {
      if (this->interrupt_pin_ == nullptr)
        return;
      if (this->interrupt_pin_->digital_read() == false && !g_int_watch_saw_low) {
        g_int_watch_saw_low = true;
        ESP_LOGW(TAG, "INT-WATCH: GPIO14 went LOW!");
      }
    });
    this->set_timeout("mcp_int_watch_stop", 30000, [this]() {
      this->cancel_interval("mcp_int_watch");
      ESP_LOGW(TAG,
               "INT-WATCH: done. GPIO14 ever LOW during window = %s. If NO, the "
               "chip INT is not reaching the ESP through the isolator (hardware).",
               g_int_watch_saw_low ? "YES" : "NO");
    });
  });
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

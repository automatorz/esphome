#pragma once

// NIT2: millis() is declared in esphome/core/hal.h, which is included
// transitively through esphome/core/component.h; no explicit hal.h include is
// required (verified against shutter_hub/shutter_group.cpp, which calls millis()
// with only the component.h include chain).
#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <vector>
#include <cstdint>

namespace esphome {
namespace ha_light_hub {

// One HaLightDimmer controls a single EXTERNAL Home Assistant light entity by
// issuing light.turn_on / light.turn_off HA API actions. Physical MCP23017
// input buttons (resolved to existing MCP23017BinarySensor vars) notify the
// dimmer on their OFF->ON press edge via a generic push callback. Current
// brightness/state is read from existing homeassistant sensor/text_sensor
// entities. Fully event-driven: no polling; double-click uses the scheduler.
class HaLightDimmer : public Component {
 public:
  void set_entity_id(const char *e) { this->entity_id_ = e; }  // flash string literal
  void set_brightness_sensor(sensor::Sensor *s) { this->brightness_ = s; }
  void set_state_sensor(text_sensor::TextSensor *s) { this->state_ = s; }
  void set_levels(std::vector<uint8_t> lv) { this->levels_ = std::move(lv); }  // ascending %
  void set_transition(float sec) { this->transition_s_ = sec; }
  void set_double_click_window(uint32_t ms) { this->dbl_window_ms_ = ms; }
  void set_debounce(uint32_t ms) { this->debounce_ms_ = ms; }

  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }
  void dump_config() override;

  // Press hooks, bound to each button in codegen via add_on_press(...).
  void on_up_pressed();
  void on_down_pressed();

 protected:
  int current_pct_();               // current level 0..100 (with pending override)
  void call_turn_on_(uint8_t pct);  // light.turn_on brightness_pct + transition
  void call_turn_off_();            // light.turn_off transition
  void fire_down_single_();         // scheduler callback -> turn off
  void fire_down_double_();         // -> turn on at min level
  static uint8_t next_level_(int cur, const std::vector<uint8_t> &levels);  // pure, testable

  const char *entity_id_{""};
  sensor::Sensor *brightness_{nullptr};
  text_sensor::TextSensor *state_{nullptr};
  std::vector<uint8_t> levels_;
  float transition_s_{1.0f};
  uint32_t dbl_window_ms_{350};
  uint32_t debounce_ms_{20};  // edge time-gate
  uint32_t last_up_ms_{0};    // millis() of last ACCEPTED up edge
  uint32_t last_down_ms_{0};  // millis() of last ACCEPTED down edge
  bool down_pending_{false};  // a single-click timer is armed

  static constexpr int LEVEL_MATCH_TOL = 2;  // % tolerance for brightness round-trip

  // --- Optimistic local level tracking (design review Finding 1) ---------
  // The HA brightness/state sensors are CACHED and lag our commands by a full
  // HA round-trip (network + HA processing). Without a local record, two quick
  // up-presses would both read the same stale level and command the SAME level,
  // stalling instead of climbing 5->45->100. So we remember the level we last
  // commanded and, during a short trust window (PENDING_TTL_MS, i.e. transition
  // + network/HA round-trip margin), treat it as authoritative in current_pct_().
  // After the window elapses we resync to the real HA state so external changes
  // win. Net: rapid up-presses climb reliably; after PENDING_TTL_MS the dimmer
  // reflects actual HA state again.
  int pending_pct_{-1};         // last brightness_pct we commanded, or -1 if none
  uint32_t pending_set_ms_{0};  // millis() when that command was issued
  static constexpr uint32_t PENDING_TTL_MS = 2000;  // trust window (transition + round-trip margin)
};

}  // namespace ha_light_hub
}  // namespace esphome

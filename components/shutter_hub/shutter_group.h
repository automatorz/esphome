#pragma once
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/cover/cover.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "shutter.h"
#include <vector>

namespace esphome { namespace shutter_hub {

class ShutterHub;

class ShutterGroup : public cover::Cover, public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  cover::CoverTraits get_traits() override;
  void control(const cover::CoverCall &call) override;

  void set_hub(ShutterHub *h) { hub_ = h; }
  // Multiple wall switches per direction: a press on ANY up switch feeds the
  // shared up click-counter (likewise for down). Added one at a time at codegen.
  void add_wall_up(binary_sensor::BinarySensor *b) { up_btns_.push_back(b); }
  void add_wall_down(binary_sensor::BinarySensor *b) { down_btns_.push_back(b); }
  void set_click_window(uint32_t ms) { click_window_ms_ = ms; }
  void set_debounce(uint32_t ms) { debounce_ms_ = ms; }
  void add_shutter(Shutter *s) { shutters_.push_back(s); }
  const std::vector<Shutter *> &shutters() const { return shutters_; }

  // Sun-automation config (relocated from Shutter). A group owns the sun
  // behavior and the window geometry it depends on; the member shutters move
  // one at a time through the hub queue.
  void set_open_at_sunrise(bool v) { open_at_sunrise_ = v; }
  void set_close_at_sunset(bool v) { close_at_sunset_ = v; }
  void set_sunrise_offset(uint32_t ms) { sunrise_off_ms_ = ms; }
  void set_sunset_offset(uint32_t ms) { sunset_off_ms_ = ms; }
  void set_suppress_sunrise_if_sun_through(bool v) { suppress_sunrise_ = v; }
  void set_close_on_sun_through(bool v) { close_on_sun_through_ = v; }
  void set_sun_through_sensor(binary_sensor::BinarySensor *b) { sun_through_sensor_ = b; }
  void set_overridden_sensor(binary_sensor::BinarySensor *b) { overridden_sensor_ = b; }
  void set_window(float w, float d, float h, WindowOrientation o, float margin) {
    window_.w = w; window_.d = d; window_.h = h;
    window_.o = o; window_.margin = margin;
    has_window_ = true;
  }

  bool open_at_sunrise() const { return open_at_sunrise_; }
  bool close_at_sunset() const { return close_at_sunset_; }
  uint32_t sunrise_offset_ms() const { return sunrise_off_ms_; }
  uint32_t sunset_offset_ms() const { return sunset_off_ms_; }
  bool suppress_sunrise_if_sun_through() const { return suppress_sunrise_; }
  bool close_on_sun_through() const { return close_on_sun_through_; }
  bool has_window() const { return has_window_; }
  float elevation_margin() const { return window_.margin; }
  bool compute_sun_through_raw(float az, float el) const;
  void publish_sun_through(bool on);

  // "Overridden today": a manual command on this group has suppressed one of
  // its sun automations for the day. Reuses the existing daily-cancel flags;
  // closed_by_sun_through is deliberately excluded (it is an automation state,
  // not a manual override).
  bool is_overridden() const {
    return sunrise_cancelled_today || sunset_cancelled_today ||
           sun_through_cancelled_today;
  }
  void publish_overridden();

  // Daily-cancel flags (relocated from Shutter). A manual command on this
  // group sets these; reset_daily_flags() clears them.
  bool sunrise_cancelled_today{false};
  bool sunset_cancelled_today{false};
  bool sun_through_cancelled_today{false};
  bool closed_by_sun_through{false};

  // Previous sun-through "active" reading, so the reopen fires only on a true
  // active->inactive EDGE (sun-through actually ended) rather than on any
  // standalone inactive reading. Starts false so that at boot, or while the sun
  // sensors are still settling (az/el briefly NaN -> sun_through_active()
  // returns false), there is NO edge and therefore NO spurious reopen. A
  // sun-through-closed shutter that reboots thus stays closed instead of
  // briefly opening then re-closing. NOT reset by reset_daily_flags() — it
  // tracks the physical sun state, not a daily decision.
  bool sun_through_prev_active_{false};

  void update_state(bool force = false);

  // Edge entry points: the coordinator fires the wall switch binary_sensor's
  // on_press_ callback on every OFF->ON press edge the instant it occurs.
  // Codegen binds each up/down switch's add_on_press to these (mirroring the
  // dimmer's on_up_pressed/on_down_pressed), so a bouncy press's brief ON can
  // no longer fall between two main-loop level samples and be lost.
  void on_wall_up_edge();
  void on_wall_down_edge();

 private:
  struct ClickState {
    uint8_t count{0};
    uint32_t last_press_ms{0};
    // Timestamp of the last ACCEPTED edge for this direction, used by the
    // debounce gate to drop the intra-burst bounce edges of a single press.
    uint32_t last_accepted_ms{0};
  };

  void accept_edge_(ClickState &cs, bool is_up, uint32_t now);
  void loop_click_(ClickState &cs, bool is_up, uint32_t now);
  void fire_click_(uint8_t count, bool is_up);

  ShutterHub *hub_{nullptr};
  // One or more wall switches per direction. The group's loop ORs each list's
  // member states into a single logical up/down signal, so a press on any one
  // switch drives the shared click-counter.
  std::vector<binary_sensor::BinarySensor *> up_btns_, down_btns_;
  binary_sensor::BinarySensor *sun_through_sensor_{nullptr};
  binary_sensor::BinarySensor *overridden_sensor_{nullptr};
  std::vector<Shutter *> shutters_;
  bool open_at_sunrise_{false}, close_at_sunset_{false};
  uint32_t sunrise_off_ms_{0}, sunset_off_ms_{0};
  bool suppress_sunrise_{true};
  bool close_on_sun_through_{true};
  struct { float w, d, h; WindowOrientation o; float margin; } window_{};
  bool has_window_{false};
  uint32_t click_window_ms_{400};
  // Edge-debounce gate: an accepted press edge locks out further edges FOR THAT
  // DIRECTION for debounce_ms_, so a single press's ~40-60ms bounce burst
  // (including an ON->OFF->ON rebounce that would otherwise publish a SECOND
  // OFF->ON logical edge) collapses to exactly ONE click, while two deliberate
  // presses ~200-400ms apart are both accepted. Default 80ms: above the observed
  // ~40-60ms bounce-burst width (with margin), well below the human double-click
  // gap. Per-group tunable via the `debounce:` YAML key.
  uint32_t debounce_ms_{80};
  ClickState up_cs_, down_cs_;
};

}}  // namespace shutter_hub
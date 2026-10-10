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
  void set_wall_up(binary_sensor::BinarySensor *b) { up_btn_ = b; }
  void set_wall_down(binary_sensor::BinarySensor *b) { down_btn_ = b; }
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

  // Daily-cancel flags (relocated from Shutter). A manual command on this
  // group sets these; reset_daily_flags() clears them.
  bool sunrise_cancelled_today{false};
  bool sunset_cancelled_today{false};
  bool sun_through_cancelled_today{false};
  bool closed_by_sun_through{false};

  void update_state(bool force = false);

 private:
  struct ClickState {
    uint8_t count{0};
    uint32_t last_press_ms{0};
  };

  void on_press_(ClickState &cs, bool is_up, uint32_t now);
  void loop_click_(ClickState &cs, bool is_up, uint32_t now);
  void fire_click_(uint8_t count, bool is_up);

  ShutterHub *hub_{nullptr};
  binary_sensor::BinarySensor *up_btn_{nullptr}, *down_btn_{nullptr};
  binary_sensor::BinarySensor *sun_through_sensor_{nullptr};
  std::vector<Shutter *> shutters_;
  bool open_at_sunrise_{false}, close_at_sunset_{false};
  uint32_t sunrise_off_ms_{0}, sunset_off_ms_{0};
  bool suppress_sunrise_{true};
  bool close_on_sun_through_{true};
  struct { float w, d, h; WindowOrientation o; float margin; } window_{};
  bool has_window_{false};
  uint32_t click_window_ms_{400};
  uint32_t debounce_ms_{20};
  ClickState up_cs_, down_cs_;
  bool up_prev_{false}, down_prev_{false};

  // Debounce state per button: the last raw reading, the millis timestamp at
  // which that raw reading was first observed, and the debounced (stable)
  // value promoted once a raw reading has held for >= debounce_ms_.
  bool up_raw_prev_{false}, down_raw_prev_{false};
  uint32_t up_raw_changed_ms_{0}, down_raw_changed_ms_{0};
  bool up_stable_{false}, down_stable_{false};

  bool debounce_(bool raw, bool &raw_prev, uint32_t &raw_changed_ms,
                 bool &stable, uint32_t now);
};

}}  // namespace shutter_hub
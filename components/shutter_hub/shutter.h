#pragma once
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/cover/cover.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include <cstdint>

namespace esphome { namespace shutter_hub {

class ShutterHub;

enum class Dir : uint8_t { NONE, UP, DOWN };
enum class WindowOrientation : uint8_t { EAST, WEST };

class Shutter : public cover::Cover, public Component {
 public:
  void setup() override;
  void dump_config() override;
  cover::CoverTraits get_traits() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_hub(ShutterHub *h) { hub_ = h; }
  void set_index(size_t i) { index_ = i; }
  void set_up_relay(switch_::Switch *s) { up_ = s; }
  void set_down_relay(switch_::Switch *s) { down_ = s; }
  void set_travel_time(uint32_t ms) { travel_ms_ = ms; }
  void set_open_at_sunrise(bool v) { open_at_sunrise_ = v; }
  void set_close_at_sunset(bool v) { close_at_sunset_ = v; }
  void set_sunrise_offset(uint32_t ms) { sunrise_off_ms_ = ms; }
  void set_sunset_offset(uint32_t ms) { sunset_off_ms_ = ms; }
  void set_suppress_sunrise_if_sun_through(bool v) { suppress_sunrise_ = v; }
  void set_close_on_sun_through(bool v) { close_on_sun_through_ = v; }
  void set_restore(bool v) { restore_ = v; }
  void set_sun_through_sensor(binary_sensor::BinarySensor *b) { sun_through_sensor_ = b; }
  void set_window(float w, float d, float h, WindowOrientation o, float margin) {
    window_.w = w; window_.d = d; window_.h = h;
    window_.o = o; window_.margin = margin;
    has_window_ = true;
  }

  uint32_t travel_time_ms() const { return travel_ms_; }
  bool open_at_sunrise() const { return open_at_sunrise_; }
  bool close_at_sunset() const { return close_at_sunset_; }
  uint32_t sunrise_offset_ms() const { return sunrise_off_ms_; }
  uint32_t sunset_offset_ms() const { return sunset_off_ms_; }
  bool suppress_sunrise_if_sun_through() const { return suppress_sunrise_; }
  bool close_on_sun_through() const { return close_on_sun_through_; }
  bool has_window() const { return has_window_; }
  float elevation_margin() const { return window_.margin; }

  void energize(Dir d);
  void de_energize();
  void publish_state_from_position();
  void publish_sun_through(bool on);

  void save_position();
  void restore_position();
  void apply_loaded_position();   // <-- NEW

  void apply_manual_command(Dir d);
  void control(const cover::CoverCall &call) override;

  bool compute_sun_through_raw(float az, float el) const;

  bool position_known{false};

  bool sunrise_cancelled_today{false};
  bool sunset_cancelled_today{false};
  bool sun_through_cancelled_today{false};
  bool closed_by_sun_through{false};

 private:
  ShutterHub *hub_{nullptr};
  switch_::Switch *up_{nullptr}, *down_{nullptr};
  binary_sensor::BinarySensor *sun_through_sensor_{nullptr};
  size_t   index_{0};
  uint32_t travel_ms_{30000};
  bool open_at_sunrise_{false}, close_at_sunset_{false};
  uint32_t sunrise_off_ms_{0}, sunset_off_ms_{0};
  bool suppress_sunrise_{true};
  bool close_on_sun_through_{true};
  bool restore_{true};

  struct { float w, d, h; WindowOrientation o; float margin; } window_{};
  bool has_window_{false};
};

}}  // namespace shutter_hub
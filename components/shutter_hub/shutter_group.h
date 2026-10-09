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
  std::vector<Shutter *> shutters_;
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
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
  void add_shutter(Shutter *s) { shutters_.push_back(s); }
  const std::vector<Shutter *> &shutters() const { return shutters_; }

  void update_state(bool force = false);

 private:
  struct ClickState {
    uint8_t count{0};
    uint32_t last_press_ms{0};
  };

  void on_press_(ClickState &cs, bool is_up);
  void loop_click_(ClickState &cs, bool is_up);
  void fire_click_(uint8_t count, bool is_up);

  ShutterHub *hub_{nullptr};
  binary_sensor::BinarySensor *up_btn_{nullptr}, *down_btn_{nullptr};
  std::vector<Shutter *> shutters_;
  uint32_t click_window_ms_{400};
  ClickState up_cs_, down_cs_;
  bool up_prev_{false}, down_prev_{false};
};

}}  // namespace shutter_hub
#pragma once
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/cover/cover.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/sensor/sensor.h"
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
  void set_restore(bool v) { restore_ = v; }
  void set_status_sensor(text_sensor::TextSensor *s) { status_sensor_ = s; }
  void set_position_sensor(sensor::Sensor *s) { position_sensor_ = s; }

  uint32_t travel_time_ms() const { return travel_ms_; }

  void energize(Dir d);
  void de_energize();
  void publish_state_from_position();

  // Telemetry: mark this shutter as queued (pending, not yet the active
  // mover) so publish_status() can report queued_open / queued_close.
  void set_queued(bool q, Dir d) { queued_ = q; queued_dir_ = d; }
  void publish_status();
  void publish_position_sensor();

  void save_position();
  void restore_position();
  void apply_loaded_position();   // <-- NEW

  void apply_manual_command(Dir d);
  void control(const cover::CoverCall &call) override;

  bool position_known{false};

 private:
  ShutterHub *hub_{nullptr};
  switch_::Switch *up_{nullptr}, *down_{nullptr};
  text_sensor::TextSensor *status_sensor_{nullptr};
  sensor::Sensor *position_sensor_{nullptr};
  bool queued_{false};
  Dir  queued_dir_{Dir::NONE};
  size_t   index_{0};
  uint32_t travel_ms_{30000};
  bool restore_{true};
};

}}  // namespace shutter_hub
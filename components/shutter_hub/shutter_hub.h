#pragma once
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/core/hal.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/time/real_time_clock.h"
#include "shutter.h"
#include <deque>
#include <vector>
#include <cmath>

namespace esphome { namespace shutter_hub {

class ShutterGroup;

struct Request {
  Shutter *shutter;
  Dir dir;
  bool from_wall_switch;
};

static constexpr size_t MAX_SHUTTER_POSITIONS = 32;
struct ShutterPositionBlock {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  float positions[MAX_SHUTTER_POSITIONS];
};

class ShutterHub : public Component {
 public:
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::BUS; }

  void add_shutter(Shutter *s) { shutters_.push_back(s); }
  void add_group(ShutterGroup *g) { groups_.push_back(g); }
  void set_sun_azimuth(sensor::Sensor *s) { sun_az_ = s; }
  void set_sun_elevation(sensor::Sensor *s) { sun_el_ = s; }
  void set_heating_needed(binary_sensor::BinarySensor *s) { heat_ = s; }
  void set_time(time::RealTimeClock *t) { time_ = t; }

  // Feature 1: scheduled backup open. hour/minute is the local time-of-day at
  // which groups that close at sunset but do NOT open at sunrise are opened as
  // a backup, once per day. Unset (hour == 255) leaves the feature inert.
  void set_backup_open_time(uint8_t hour, uint8_t minute) {
    backup_open_hour_ = hour;
    backup_open_minute_ = minute;
  }

  // Feature 2: open every currently-closed shutter across all groups, skipping
  // groups closed by sun-through or already opened/cancelled today. Driven by
  // the Open-All button entity.
  void open_all_closed_no_override();

  void enqueue(Shutter *s, Dir d, bool from_wall_switch);
  void stop_active();
  void cancel_shutter(Shutter *s);
  void cancel_group(ShutterGroup *g);

  void update_group_states();
  void update_group_states_for(Shutter *s);

  void schedule_sunrise_open();
  void schedule_sunset_close();
  void on_sun_update();
  bool sun_through_active(ShutterGroup *g);
  bool heating_needed_on() const;
  float sun_azimuth() const;
  float sun_elevation() const;

  void reset_daily_flags();

  Shutter *active_shutter() const { return active_; }

  float get_restored_position(size_t index, bool &found);
  void  set_shutter_position(size_t index, float value);

 private:
  enum class HubState : uint8_t { IDLE, SETTLING, MOVING, COOLDOWN };

  void begin_request_();
  void complete_active_();
  void finalize_active_(Shutter *finished);
  void refresh_active_position_();
  void ensure_positions_loaded_();
  void maybe_backup_open_();

  HubState state_{HubState::IDLE};
  uint32_t state_start_ms_{0};

  std::deque<Request> queue_;
  Shutter *active_{nullptr};
  Dir active_dir_{Dir::NONE};
  uint32_t active_duration_ms_{0};
  float    active_start_pos_{1.0f};

  uint32_t last_pos_publish_ms_{0};
  float    last_group_update_pos_{1.0f};

  std::vector<Shutter *> shutters_;
  std::vector<ShutterGroup *> groups_;
  sensor::Sensor *sun_az_{nullptr};
  sensor::Sensor *sun_el_{nullptr};
  binary_sensor::BinarySensor *heat_{nullptr};
  time::RealTimeClock *time_{nullptr};

  bool boot_sunset_check_done_{false};

  // Feature 1 state. hour == 255 means backup_open_time was never configured,
  // so the feature is inert and loop() early-returns before reading the clock.
  uint8_t backup_open_hour_{255};
  uint8_t backup_open_minute_{0};
  bool    backup_open_fired_today_{false};

  // Day-rollover detector for the internal daily reset. When the calendar day
  // changes, loop() calls reset_daily_flags() once (no external midnight
  // on_time trigger needed). -1 until the first valid clock read.
  int16_t reset_last_day_{-1};

  ESPPreferenceObject positions_pref_;
  ShutterPositionBlock positions_block_;
  bool positions_loaded_{false};
  bool positions_pref_made_{false};
  uint8_t positions_load_attempts_{0};
};

}}  // namespace shutter_hub
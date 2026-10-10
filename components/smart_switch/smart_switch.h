#pragma once

#include "esphome/core/component.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/time/real_time_clock.h"

namespace esphome {
namespace smart_switch {

// ---------------------------------------------------------------------------
// Schedule configuration
// ---------------------------------------------------------------------------
struct ScheduleConfig {
  bool enabled{false};
  uint32_t start_offset{0};   // seconds; phase offset from 1970-01-01 local
  uint32_t period{0};         // seconds; may exceed 24h (e.g. 604800 = 1 week)
  uint32_t duration{0};       // seconds the target is "active" within each cycle
  bool invert{false};
  time::RealTimeClock *time{nullptr};
};

// ---------------------------------------------------------------------------
// SmartSwitch — supervises another switch. Not published to HA.
// ---------------------------------------------------------------------------
class SmartSwitch : public Component {
 public:
  // --- Config setters -----------------------------------------------------
  void set_target_switch(switch_::Switch *sw) { this->target_switch_ = sw; }
  void set_debug(bool dbg) { this->debug_ = dbg; }
  void set_initial_state(bool state) { this->initial_state_ = state; }
  void set_force_initial_state(bool force) { this->force_initial_state_ = force; }
  void set_update_interval(uint32_t ms) { this->update_interval_ = ms; }

  void set_schedule_enabled(bool v)               { this->schedule_.enabled = v; }
  void set_schedule_start_offset(uint32_t s)      { this->schedule_.start_offset = s; }
  void set_schedule_period(uint32_t s)            { this->schedule_.period = s; }
  void set_schedule_duration(uint32_t s)          { this->schedule_.duration = s; }
  void set_schedule_invert(bool v)                { this->schedule_.invert = v; }
  void set_schedule_time(time::RealTimeClock *t)  { this->schedule_.time = t; }

  // --- Component lifecycle ------------------------------------------------
  void setup() override {
    this->apply_state_(this->initial_state_, "initial state");

    // Only start the periodic loop if there is actually periodic work to do.
    bool needs_loop = this->force_initial_state_ || this->schedule_.enabled;
    if (needs_loop) {
      this->set_interval("smart_switch_loop", this->update_interval_,
                         [this]() { this->loop_(); });
    } else if (this->debug_) {
      ESP_LOGD(TAG, "No force_initial_state and no schedule — loop disabled");
    }
  }

  void dump_config() override {
    ESP_LOGCONFIG(TAG, "Smart Switch:");
    ESP_LOGCONFIG(TAG, "  Target switch: %s",
                  this->target_switch_->get_name().c_str());
    ESP_LOGCONFIG(TAG, "  Initial state: %s", ONOFF(this->initial_state_));
    ESP_LOGCONFIG(TAG, "  Force initial state: %s",
                  YESNO(this->force_initial_state_));
    ESP_LOGCONFIG(TAG, "  Update interval: %u ms", this->update_interval_);
    if (this->schedule_.enabled) {
      ESP_LOGCONFIG(TAG, "  Schedule:");
      ESP_LOGCONFIG(TAG, "    Start offset: %u s", this->schedule_.start_offset);
      ESP_LOGCONFIG(TAG, "    Period: %u s", this->schedule_.period);
      ESP_LOGCONFIG(TAG, "    Duration: %u s", this->schedule_.duration);
      ESP_LOGCONFIG(TAG, "    Invert: %s", YESNO(this->schedule_.invert));
    }
  }

 protected:
  // -----------------------------------------------------------------------
  // Howard Hinnant's days_from_civil — days since 1970-01-01 for a Y/M/D.
  // -----------------------------------------------------------------------
  static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097LL + static_cast<int64_t>(doe) - 719468LL;
  }

  // -----------------------------------------------------------------------
  // Main loop
  // -----------------------------------------------------------------------
  void loop_() {
    if (this->force_initial_state_) {
      this->apply_state_(this->initial_state_, "force initial state");
    }
    if (this->schedule_.enabled) {
      this->process_schedule_();
    }
  }

  // -----------------------------------------------------------------------
  // Schedule evaluation — fully epoch-anchored, restart-stable
  // -----------------------------------------------------------------------
  void process_schedule_() {
    auto *time = this->schedule_.time;
    auto now = time->now();
    if (!now.is_valid()) {
      if (this->debug_) {
        ESP_LOGD(TAG, "Time is invalid — leaving target switch unchanged");
      }
      return;
    }

    // "Local epoch": seconds since 1970-01-01 interpreted in local time.
    int64_t days = days_from_civil(now.year, now.month, now.day_of_month);
    int64_t time_of_day = static_cast<int64_t>(now.hour)   * 3600LL
                        + static_cast<int64_t>(now.minute) * 60LL
                        + static_cast<int64_t>(now.second);
    int64_t local_epoch = days * 86400LL + time_of_day;

    uint32_t period = this->schedule_.period;
    if (period == 0) return;

    int64_t shifted = local_epoch - static_cast<int64_t>(this->schedule_.start_offset);
    int64_t position = ((shifted % static_cast<int64_t>(period))
                        + static_cast<int64_t>(period))
                       % static_cast<int64_t>(period);

    bool active = static_cast<uint32_t>(position) < this->schedule_.duration;
    bool target = this->schedule_.invert ? !active : active;

    this->apply_state_(target, "schedule");
  }

  // -----------------------------------------------------------------------
  // Helper
  // -----------------------------------------------------------------------
  void apply_state_(bool state, const char *reason) {
    if (this->target_switch_->state == state) return;

    if (this->debug_) {
      ESP_LOGD(TAG, "Setting target switch '%s' to %s (%s)",
               this->target_switch_->get_name().c_str(), ONOFF(state), reason);
    }

    if (state) this->target_switch_->turn_on();
    else       this->target_switch_->turn_off();
  }

  // --- Members ------------------------------------------------------------
  switch_::Switch *target_switch_{nullptr};
  bool debug_{false};
  bool initial_state_{false};        // default: off
  bool force_initial_state_{false};
  uint32_t update_interval_{10000};  // ms; default 10s
  ScheduleConfig schedule_{};

  static const char *const TAG;
};

const char *const SmartSwitch::TAG = "smart_switch";

}  // namespace smart_switch
}  // namespace esphome

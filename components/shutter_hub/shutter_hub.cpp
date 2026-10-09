#include "shutter_hub.h"
#include "shutter.h"
#include "shutter_group.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/application.h"
#include <algorithm>
#include <cstring>

namespace esphome { namespace shutter_hub {

static const char *TAG = "shutter_hub";

static constexpr uint32_t POS_PUBLISH_INTERVAL_MS = 750;
static constexpr float GROUP_UPDATE_MIN_DELTA = 0.01f;
static constexpr uint32_t SETTLE_MS = 200;

// Quiet gap between consecutive queued movements. Gives the I2C bus a
// window where the ESP32 is not issuing writes and can recover from any
// stalled transaction before the next relay is driven.
static constexpr uint32_t INTER_REQUEST_DELAY_MS = 2000;

static constexpr uint32_t POSITIONS_MAGIC = 0x53485554;  // "SHUT"
static constexpr uint32_t POSITIONS_PREF_HASH = 0x5A00F00D;

void ShutterHub::setup() {
  ESP_LOGCONFIG(TAG, "Shutter hub ready: %u shutters, %u groups",
                (unsigned) shutters_.size(), (unsigned) groups_.size());
}

void ShutterHub::ensure_positions_loaded_() {
  if (positions_loaded_) return;

  if (!positions_pref_made_) {
    positions_pref_ = global_preferences->make_preference<ShutterPositionBlock>(
        POSITIONS_PREF_HASH);
    positions_pref_made_ = true;
    for (size_t i = 0; i < MAX_SHUTTER_POSITIONS; i++) {
      positions_block_.positions[i] = NAN;
    }
    positions_block_.magic = POSITIONS_MAGIC;
  }

  ShutterPositionBlock loaded;
  memset(&loaded, 0, sizeof(loaded));
  bool loaded_ok = positions_pref_.load(&loaded);

  if (!loaded_ok) {
    positions_load_attempts_++;
    if (positions_load_attempts_ < 5) {
      ESP_LOGD(TAG, "RESTORE: load attempt %u failed, retrying next loop",
               (unsigned) positions_load_attempts_);
      return;
    }
    ESP_LOGW(TAG, "RESTORE: no saved positions after %u attempts, starting fresh",
             (unsigned) positions_load_attempts_);
    positions_loaded_ = true;
    return;
  }

  positions_loaded_ = true;
  if (loaded.magic == POSITIONS_MAGIC) {
    memcpy(&positions_block_, &loaded, sizeof(positions_block_));
    ESP_LOGI(TAG, "RESTORE: loaded positions block from NVS");
    for (size_t i = 0; i < shutters_.size() && i < MAX_SHUTTER_POSITIONS; i++) {
      ESP_LOGI(TAG, "RESTORE:   shutter[%u] = %.3f",
               (unsigned) i, positions_block_.positions[i]);
    }
  } else {
    ESP_LOGW(TAG, "RESTORE: empty/invalid block (magic=0x%08X), starting fresh",
             (unsigned) loaded.magic);
  }
}

float ShutterHub::get_restored_position(size_t index, bool &found) {
  found = false;
  this->ensure_positions_loaded_();
  if (index >= MAX_SHUTTER_POSITIONS) return 1.0f;
  float p = positions_block_.positions[index];
  if (std::isnan(p)) return 1.0f;
  found = true;
  return p;
}

void ShutterHub::set_shutter_position(size_t index, float value) {
  this->ensure_positions_loaded_();
  if (index >= MAX_SHUTTER_POSITIONS) return;
  if (!std::isfinite(value)) return;
  positions_block_.positions[index] = value;
  bool ok = positions_pref_.save(&positions_block_);
  ESP_LOGD(TAG, "saved index=%u val=%.3f ok=%d",
           (unsigned) index, value, (int) ok);
}

void ShutterHub::loop() {
  if (!positions_loaded_) {
    this->ensure_positions_loaded_();
    if (positions_loaded_) {
      for (auto *s : shutters_) s->apply_loaded_position();
      // Shutters now have their restored positions and position_known=true.
      // Re-aggregate every group so groups reflect the real state instead of
      // the "open" default published from ShutterGroup::setup() (which ran
      // before any position was known).
      this->update_group_states();
    }
    return;
  }

  if (!boot_sunset_check_done_) {
    float el = this->sun_elevation();
    if (!std::isnan(el)) {
      boot_sunset_check_done_ = true;
      if (el <= 0.0f) {
        ESP_LOGD(TAG, "Boot: sun below horizon (el=%.1f), enforcing sunset close", el);
        for (auto *s : shutters_) {
          if (!s->close_at_sunset() || s->sunset_cancelled_today) continue;
          if (s->position_known && s->position <= 0.01f) continue;
          this->enqueue(s, Dir::DOWN, false);
        }
      }
    }
  }

  switch (state_) {
    case HubState::IDLE:
      if (!queue_.empty()) this->begin_request_();
      break;

    case HubState::SETTLING:
      if ((uint32_t)(millis() - state_start_ms_) >= SETTLE_MS) {
        state_ = HubState::MOVING;
        state_start_ms_ = millis();
        last_pos_publish_ms_ = millis() - POS_PUBLISH_INTERVAL_MS;
        last_group_update_pos_ = active_start_pos_;
        if (active_ != nullptr) active_->publish_state(false);
      }
      break;

    case HubState::MOVING:
      if ((uint32_t)(millis() - state_start_ms_) >= active_duration_ms_) {
        this->complete_active_();
      } else {
        this->refresh_active_position_();
      }
      break;

    case HubState::COOLDOWN:
      if ((uint32_t)(millis() - state_start_ms_) >= INTER_REQUEST_DELAY_MS) {
        state_ = HubState::IDLE;
      }
      break;
  }
}

void ShutterHub::begin_request_() {
  Request r = queue_.front();
  queue_.pop_front();
  active_ = r.shutter;
  active_dir_ = r.dir;
  active_was_resume_ = r.is_resume;
  active_start_pos_ = active_->position_known ? active_->position
                       : (r.dir == Dir::UP ? 0.0f : 1.0f);
  float target = (r.dir == Dir::UP) ? 1.0f : 0.0f;
  float delta = std::fabs(target - active_start_pos_);
  active_duration_ms_ = (uint32_t)(delta * (float) active_->travel_time_ms());
  if (active_duration_ms_ < 50) active_duration_ms_ = 50;
  active_->current_operation = (r.dir == Dir::UP)
                                   ? cover::COVER_OPERATION_OPENING
                                   : cover::COVER_OPERATION_CLOSING;
  // Now the active mover: clear any queued marker so status reports
  // opening/closing rather than queued_*.
  active_->set_queued(false, Dir::NONE);
  active_->energize(r.dir);
  active_->publish_state(false);
  active_->publish_status();
  state_ = HubState::SETTLING;
  state_start_ms_ = millis();
}

void ShutterHub::refresh_active_position_() {
  if (active_ == nullptr) return;
  uint32_t elapsed = millis() - state_start_ms_;
  float frac = active_duration_ms_ == 0 ? 1.0f
              : (float) elapsed / (float) active_duration_ms_;
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  float target = (active_dir_ == Dir::UP) ? 1.0f : 0.0f;
  float new_pos = active_start_pos_ + (target - active_start_pos_) * frac;
  active_->position = new_pos;
  active_->position_known = true;
  active_->current_operation = (active_dir_ == Dir::UP)
                                   ? cover::COVER_OPERATION_OPENING
                                   : cover::COVER_OPERATION_CLOSING;
  uint32_t now = millis();
  if ((uint32_t)(now - last_pos_publish_ms_) >= POS_PUBLISH_INTERVAL_MS) {
    last_pos_publish_ms_ = now;
    active_->publish_state(false);
    active_->publish_status();
    active_->publish_position_sensor();
    float delta = std::fabs(new_pos - last_group_update_pos_);
    if (delta >= GROUP_UPDATE_MIN_DELTA) {
      last_group_update_pos_ = new_pos;
      this->update_group_states_for(active_);
    }
  }
}

void ShutterHub::enqueue(Shutter *s, Dir d, bool from_wall_switch) {
  if (from_wall_switch) {
    if (d == Dir::UP) {
      s->sunrise_cancelled_today     = true;
      s->sun_through_cancelled_today = true;
      s->closed_by_sun_through       = false;
    } else {
      s->sunset_cancelled_today = true;
    }
  }
  queue_.erase(
      std::remove_if(queue_.begin(), queue_.end(),
                     [s](const Request &r) { return r.shutter == s; }),
      queue_.end());
  if (active_ == s) {
    if (active_dir_ == d) return;
    stop_active();
  } else if (active_ != nullptr && from_wall_switch) {
    Request preempted{active_, active_dir_, false, true};
    stop_active();  // resets preempted shutter's icon to IDLE
    queue_.push_front(preempted);
    // Re-mark the preempted shutter as queued so it keeps showing its
    // (resume) direction icon while it waits for its turn again.
    preempted.shutter->current_operation =
        (preempted.dir == Dir::UP) ? cover::COVER_OPERATION_OPENING
                                   : cover::COVER_OPERATION_CLOSING;
    preempted.shutter->set_queued(true, preempted.dir);
    preempted.shutter->publish_state(false);
    preempted.shutter->publish_status();
  }
  if (from_wall_switch) queue_.push_front({s, d, true, false});
  else                  queue_.push_back({s, d, false, false});

  // Visual feedback: a shutter sitting in the queue (not yet the active one)
  // immediately shows the opening/closing icon in HA, so a batch command makes
  // it obvious which shutters have a movement pending. The icon matches the
  // queued direction; begin_request_() keeps it set when the shutter becomes
  // active, and complete_active_/stop_active/cancel reset it to IDLE.
  if (active_ != s) {
    s->current_operation = (d == Dir::UP) ? cover::COVER_OPERATION_OPENING
                                          : cover::COVER_OPERATION_CLOSING;
    s->set_queued(true, d);
    s->publish_state(false);
    s->publish_status();
  }
}

void ShutterHub::stop_active() {
  if (active_ == nullptr) return;
  if (state_ == HubState::MOVING) {
    uint32_t elapsed = millis() - state_start_ms_;
    if (elapsed > active_duration_ms_) elapsed = active_duration_ms_;
    float frac = active_duration_ms_ == 0 ? 1.0f
                : (float) elapsed / (float) active_duration_ms_;
    if (active_dir_ == Dir::UP) {
      active_->position = std::min(1.0f, active_->position + frac);
    } else if (active_dir_ == Dir::DOWN) {
      active_->position = std::max(0.0f, active_->position - frac);
    }
  }
  active_->current_operation = cover::COVER_OPERATION_IDLE;
  active_->set_queued(false, Dir::NONE);
  active_->de_energize();
  active_->publish_state_from_position();
  active_->save_position();
  Shutter *stopped = active_;
  active_ = nullptr;
  active_dir_ = Dir::NONE;
  active_was_resume_ = false;

  // If there's still work queued, give the bus a quiet window before
  // starting the next one. If the queue is empty, go straight to IDLE
  // so a fresh user command responds immediately.
  state_start_ms_ = millis();
  state_ = queue_.empty() ? HubState::IDLE : HubState::COOLDOWN;

  this->update_group_states_for(stopped);
}

void ShutterHub::cancel_shutter(Shutter *s) {
  bool was_active = (active_ == s);
  if (was_active) stop_active();  // resets its icon to IDLE already
  bool was_queued = false;
  queue_.erase(
      std::remove_if(queue_.begin(), queue_.end(),
                     [s, &was_queued](const Request &r) {
                       if (r.shutter == s) { was_queued = true; return true; }
                       return false;
                     }),
      queue_.end());
  // A shutter that was only queued (not active) was showing the pending
  // opening/closing icon from enqueue(). Clear it back to IDLE and refresh
  // its group so the UI doesn't leave it "closing" forever.
  if (!was_active && was_queued) {
    s->current_operation = cover::COVER_OPERATION_IDLE;
    s->set_queued(false, Dir::NONE);
    s->publish_state_from_position();
    this->update_group_states_for(s);
  }
}

void ShutterHub::cancel_group(ShutterGroup *g) {
  for (auto *s : g->shutters()) cancel_shutter(s);
}

void ShutterHub::complete_active_() {
  if (active_ == nullptr) return;
  if (active_dir_ == Dir::UP) active_->position = 1.0f;
  else                        active_->position = 0.0f;
  active_->position_known = true;
  active_->current_operation = cover::COVER_OPERATION_IDLE;
  active_->set_queued(false, Dir::NONE);
  active_->de_energize();
  active_->publish_state_from_position();
  active_->save_position();
  Shutter *finished = active_;
  active_ = nullptr;
  active_dir_ = Dir::NONE;
  active_was_resume_ = false;

  // Same COOLDOWN logic as stop_active().
  state_start_ms_ = millis();
  state_ = queue_.empty() ? HubState::IDLE : HubState::COOLDOWN;

  this->update_group_states_for(finished);
}

void ShutterHub::update_group_states() {
  for (auto *g : groups_) g->update_state();
}

void ShutterHub::update_group_states_for(Shutter *s) {
  if (s == nullptr) return;
  for (auto *g : groups_) {
    for (auto *member : g->shutters()) {
      if (member == s) { g->update_state(); break; }
    }
  }
}

float ShutterHub::sun_azimuth() const {
  return (sun_az_ && !std::isnan(sun_az_->state)) ? sun_az_->state : NAN;
}
float ShutterHub::sun_elevation() const {
  return (sun_el_ && !std::isnan(sun_el_->state)) ? sun_el_->state : NAN;
}
bool ShutterHub::heating_needed_on() const {
  return heat_ != nullptr && heat_->state;
}

bool ShutterHub::sun_through_active(Shutter *s) {
  if (!s->has_window()) return false;
  float az = this->sun_azimuth();
  float el = this->sun_elevation();
  if (std::isnan(az) || std::isnan(el)) return false;
  if (el < s->elevation_margin()) return false;
  return s->compute_sun_through_raw(az, el);
}

void ShutterHub::schedule_sunrise_open() {
  for (auto *s : shutters_) {
    if (!s->open_at_sunrise() || s->sunrise_cancelled_today) continue;
    if (s->suppress_sunrise_if_sun_through() && this->sun_through_active(s)) continue;
    if (s->position_known && s->position >= 0.99f) continue;
    uint32_t off = s->sunrise_offset_ms();
    if (off == 0) this->enqueue(s, Dir::UP, false);
    else this->set_timeout(off, [this, s]() { this->enqueue(s, Dir::UP, false); });
  }
}

void ShutterHub::schedule_sunset_close() {
  for (auto *s : shutters_) {
    if (!s->close_at_sunset() || s->sunset_cancelled_today) continue;
    if (s->position_known && s->position <= 0.01f) continue;
    uint32_t off = s->sunset_offset_ms();
    if (off == 0) this->enqueue(s, Dir::DOWN, false);
    else this->set_timeout(off, [this, s]() { this->enqueue(s, Dir::DOWN, false); });
  }
}

void ShutterHub::on_sun_update() {
  for (auto *s : shutters_) {
    if (!s->has_window()) continue;
    bool active = this->sun_through_active(s);
    s->publish_sun_through(active);
    if (!s->close_on_sun_through()) continue;
    if (s->sun_through_cancelled_today) continue;
    if (this->heating_needed_on()) continue;
    if (active && !s->closed_by_sun_through) {
      this->enqueue(s, Dir::DOWN, false);
      s->closed_by_sun_through = true;
    } else if (!active && s->closed_by_sun_through) {
      float el = this->sun_elevation();
      if (!s->close_at_sunset() && !std::isnan(el) &&
          el > s->elevation_margin()) {
        this->enqueue(s, Dir::UP, false);
      }
      s->closed_by_sun_through = false;
    }
  }
}

void ShutterHub::reset_daily_flags() {
  for (auto *s : shutters_) {
    s->sunrise_cancelled_today     = false;
    s->sunset_cancelled_today      = false;
    s->sun_through_cancelled_today = false;
    s->closed_by_sun_through       = false;
  }
  ESP_LOGD(TAG, "Daily flags reset");
}

}}  // namespace shutter_hub
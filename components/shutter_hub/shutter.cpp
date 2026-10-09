#include "shutter.h"
#include "shutter_hub.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/application.h"
#include <cmath>
#include <cstdio>

namespace esphome { namespace shutter_hub {

static const char *TAG = "shutter";

static constexpr uint32_t RELAY_GAP_MS = 50;

void Shutter::setup() {
  // Don't read the position here. The hub loads it on its first loop pass
  // (after all setups are done, so preferences is definitely up) and calls
  // apply_loaded_position() on each shutter.
  this->position = 1.0f;
  this->position_known = false;
  this->current_operation = cover::COVER_OPERATION_IDLE;
  this->publish_state_from_position();
}

void Shutter::apply_loaded_position() {
  if (hub_ == nullptr) {
    ESP_LOGW(TAG, "'%s': RESTORE: hub is null, cannot restore",
             this->get_name().c_str());
    return;
  }
  if (!restore_) {
    ESP_LOGI(TAG, "'%s': RESTORE: restore disabled (index=%u)",
             this->get_name().c_str(), (unsigned) index_);
    return;
  }
  bool found = false;
  float p = hub_->get_restored_position(index_, found);
  if (found) {
    this->position = p;
    this->position_known = true;
    ESP_LOGI(TAG, "'%s': RESTORE: position=%.3f (index=%u)",
             this->get_name().c_str(), p, (unsigned) index_);
  } else {
    this->position = 1.0f;
    this->position_known = false;
    ESP_LOGW(TAG, "'%s': RESTORE: no saved position (index=%u), assuming open/unknown",
             this->get_name().c_str(), (unsigned) index_);
  }
  this->current_operation = cover::COVER_OPERATION_IDLE;
  this->publish_state_from_position();
}

void Shutter::dump_config() {
  ESP_LOGCONFIG(TAG, "Shutter '%s' travel=%ums open_at_sunrise=%d close_at_sunset=%d window=%d index=%u",
                this->get_name().c_str(), (unsigned) travel_ms_,
                open_at_sunrise_, close_at_sunset_, has_window_,
                (unsigned) index_);
}

cover::CoverTraits Shutter::get_traits() {
  auto t = cover::CoverTraits();
  t.set_is_assumed_state(true);
  t.set_supports_position(false);
  t.set_supports_tilt(false);
  t.set_supports_stop(true);
  return t;
}

void Shutter::apply_manual_command(Dir d) {
  if (d == Dir::UP) {
    sunrise_cancelled_today     = true;
    sun_through_cancelled_today = true;
    closed_by_sun_through       = false;
  } else if (d == Dir::DOWN) {
    sunset_cancelled_today = true;
  }
  if (hub_ != nullptr) hub_->enqueue(this, d, false);
}

void Shutter::control(const cover::CoverCall &call) {
  if (call.get_stop()) {
    if (hub_ != nullptr) hub_->cancel_shutter(this);
    return;
  }
  if (call.get_position().has_value()) {
    float p = *call.get_position();
    Dir d = (p > this->position + 0.01f) ? Dir::UP : Dir::DOWN;
    this->apply_manual_command(d);
  }
}

void Shutter::energize(Dir d) {
  switch_::Switch *to_off = nullptr;
  switch_::Switch *to_on  = nullptr;
  if (d == Dir::UP) { to_off = down_; to_on = up_; }
  else if (d == Dir::DOWN) { to_off = up_; to_on = down_; }
  else return;
  if (to_off != nullptr && to_off->state) to_off->turn_off();
  if (to_on == nullptr) return;
  Dir wanted = d;
  Shutter *self = this;
  this->set_timeout(RELAY_GAP_MS, [self, wanted]() {
    ShutterHub *hub = self->hub_;
    if (hub == nullptr) return;
    if (hub->active_shutter() != self) return;
    switch_::Switch *target = (wanted == Dir::UP) ? self->up_ : self->down_;
    if (target != nullptr && !target->state) target->turn_on();
  });
}

void Shutter::de_energize() {
  if (up_   != nullptr && up_->state)   up_->turn_off();
  if (down_ != nullptr && down_->state) down_->turn_off();
}

void Shutter::publish_state_from_position() {
  float p = this->position_known ? this->position : 1.0f;
  if (p < 0.0f) p = 0.0f;
  if (p > 1.0f) p = 1.0f;
  this->position = p;
  this->publish_state();
  this->publish_status();
  this->publish_position_sensor();
}

void Shutter::publish_status() {
  if (status_sensor_ == nullptr) return;

  const char *status;
  bool is_active = (hub_ != nullptr && hub_->active_shutter() == this);

  if (is_active && this->current_operation == cover::COVER_OPERATION_OPENING) {
    status = "opening";
  } else if (is_active &&
             this->current_operation == cover::COVER_OPERATION_CLOSING) {
    status = "closing";
  } else if (queued_ && queued_dir_ == Dir::UP) {
    status = "queued_open";
  } else if (queued_ && queued_dir_ == Dir::DOWN) {
    status = "queued_close";
  } else {
    // Idle: derive from position.
    float p = this->position_known ? this->position : 1.0f;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    if (p >= 0.99f) {
      status = "open";
    } else if (p <= 0.01f) {
      status = "closed";
    } else {
      char buf[16];
      int pct = (int) std::lroundf(p * 100.0f);
      snprintf(buf, sizeof(buf), "partial:%d", pct);
      status_sensor_->publish_state(buf);
      return;
    }
  }
  status_sensor_->publish_state(status);
}

void Shutter::publish_position_sensor() {
  if (position_sensor_ == nullptr) return;
  float p = this->position_known ? this->position : 1.0f;
  if (p < 0.0f) p = 0.0f;
  if (p > 1.0f) p = 1.0f;
  position_sensor_->publish_state((float) std::lroundf(p * 100.0f));
}

void Shutter::publish_sun_through(bool on) {
  if (sun_through_sensor_ != nullptr) sun_through_sensor_->publish_state(on);
}

void Shutter::save_position() {
  if (!restore_) return;
  if (!std::isfinite(this->position)) {
    ESP_LOGW(TAG, "'%s': refusing to save non-finite position",
             this->get_name().c_str());
    return;
  }
  if (hub_ != nullptr) hub_->set_shutter_position(index_, this->position);
}

void Shutter::restore_position() {
  this->apply_loaded_position();
}

bool Shutter::compute_sun_through_raw(float az, float el) const {
  if (!has_window_) return false;
  if (std::isnan(az) || std::isnan(el)) return false;
  if (el <= 0.0f) return false;
  const float W = window_.w, D = window_.d, H = window_.h;
  const float HD_ratio = H / D;
  const float max_horiz_angle_deg = std::atanf(W / D) * 180.0f / (float) M_PI;
  if (window_.o == WindowOrientation::EAST) {
    if (az <= 0.0f || az >= 180.0f) return false;
    if (std::fabs(az - 90.0f) >= max_horiz_angle_deg) return false;
    float az_rad = az * (float) M_PI / 180.0f;
    float tan_el = std::tanf(el * (float) M_PI / 180.0f);
    float limit  = HD_ratio * std::sinf(az_rad);
    if (tan_el >= limit) return false;
    return true;
  } else {
    if (az <= 180.0f || az >= 360.0f) return false;
    if (std::fabs(az - 270.0f) >= max_horiz_angle_deg) return false;
    float az_rad = az * (float) M_PI / 180.0f;
    float tan_el = std::tanf(el * (float) M_PI / 180.0f);
    float limit  = HD_ratio * (-std::sinf(az_rad));
    if (tan_el >= limit) return false;
    return true;
  }
}

}}  // namespace shutter_hub
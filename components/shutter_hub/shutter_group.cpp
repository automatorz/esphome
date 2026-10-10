#include "shutter_group.h"
#include "shutter.h"
#include "shutter_hub.h"
#include "esphome/core/log.h"
#include <cmath>

namespace esphome { namespace shutter_hub {

static const char *TAG = "shutter_group";

static constexpr float POS_EPSILON = 0.005f;

void ShutterGroup::setup() {
  this->update_state(true);
}

void ShutterGroup::dump_config() {
  ESP_LOGCONFIG(TAG, "Shutter group '%s' (%u shutters, click_window=%ums)",
                this->get_name().c_str(), (unsigned) shutters_.size(),
                (unsigned) click_window_ms_);
}

cover::CoverTraits ShutterGroup::get_traits() {
  auto t = cover::CoverTraits();
  t.set_is_assumed_state(true);
  t.set_supports_position(false);
  t.set_supports_tilt(false);
  t.set_supports_stop(true);
  return t;
}

void ShutterGroup::control(const cover::CoverCall &call) {
  if (call.get_stop()) {
    if (hub_ != nullptr) hub_->cancel_group(this);
    return;
  }
  if (call.get_position().has_value()) {
    float p = *call.get_position();
    Dir d = (p > 0.5f) ? Dir::UP : Dir::DOWN;
    // An HA position command on the group is a manual command: cancel this
    // group's sun automation for the day, same as a wall-switch double-click.
    if (d == Dir::UP) {
      this->sunrise_cancelled_today     = true;
      this->sun_through_cancelled_today = true;
      this->closed_by_sun_through       = false;
    } else {
      this->sunset_cancelled_today = true;
    }
    for (auto *s : shutters_) s->apply_manual_command(d);
  }
}

void ShutterGroup::update_state(bool force) {
  bool all_closed = true;
  bool all_open   = true;
  bool any_known  = false;
  bool any_opening = false;
  bool any_closing = false;
  float sum = 0.0f;
  int   n   = 0;
  size_t valid = 0;

  for (auto *s : shutters_) {
    if (s == nullptr) continue;
    valid++;
    if (s->position_known) { any_known = true; sum += s->position; n++; }
    if (!(s->position <= 0.01f)) all_closed = false;
    if (!(s->position >= 0.99f)) all_open   = false;
    if (s->current_operation == cover::COVER_OPERATION_OPENING) any_opening = true;
    if (s->current_operation == cover::COVER_OPERATION_CLOSING) any_closing = true;
  }

  float new_pos;
  if (valid == 0 || !any_known) {
    new_pos = 1.0f;
  } else if (all_closed) {
    new_pos = 0.0f;
  } else if (all_open) {
    new_pos = 1.0f;
  } else {
    new_pos = (n > 0) ? (sum / (float) n) : 1.0f;
  }

  cover::CoverOperation new_op;
  if (any_opening && !any_closing)      new_op = cover::COVER_OPERATION_OPENING;
  else if (any_closing && !any_opening) new_op = cover::COVER_OPERATION_CLOSING;
  else                                  new_op = cover::COVER_OPERATION_IDLE;

  bool changed = force
              || new_op != this->current_operation
              || std::fabs(new_pos - this->position) >= POS_EPSILON;

  if (!changed) return;

  this->position = new_pos;
  this->current_operation = new_op;
  this->publish_state();
}

// Standard debounce: a raw reading must hold steady for >= debounce_ms_
// before it is promoted to the stable value. Returns the stable value.
bool ShutterGroup::debounce_(bool raw, bool &raw_prev, uint32_t &raw_changed_ms,
                             bool &stable, uint32_t now) {
  if (raw != raw_prev) {
    raw_prev = raw;
    raw_changed_ms = now;
  } else if (raw != stable &&
             (uint32_t)(now - raw_changed_ms) >= debounce_ms_) {
    stable = raw;
  }
  return stable;
}

void ShutterGroup::loop() {
  const uint32_t now = millis();

  bool u_raw = up_btn_   != nullptr && up_btn_->state;
  bool d_raw = down_btn_ != nullptr && down_btn_->state;

  // Feed the DEBOUNCED stable state into the edge/click detection below, so a
  // noisy raw reading can't register as a spurious click.
  bool u = debounce_(u_raw, up_raw_prev_, up_raw_changed_ms_, up_stable_, now);
  bool d = debounce_(d_raw, down_raw_prev_, down_raw_changed_ms_, down_stable_,
                     now);

  if (u && !up_prev_)   on_press_(up_cs_, true, now);
  if (d && !down_prev_) on_press_(down_cs_, false, now);
  up_prev_ = u;
  down_prev_ = d;

  loop_click_(up_cs_, true, now);
  loop_click_(down_cs_, false, now);
}

void ShutterGroup::on_press_(ClickState &cs, bool is_up, uint32_t now) {
  if (cs.count > 0 && (uint32_t)(now - cs.last_press_ms) > click_window_ms_) {
    cs.count = 0;
  }
  cs.count++;
  cs.last_press_ms = now;
  if (cs.count >= 3) {
    fire_click_(3, is_up);
    cs.count = 0;
  }
}

void ShutterGroup::loop_click_(ClickState &cs, bool is_up, uint32_t now) {
  if (cs.count == 0) return;
  if ((uint32_t)(now - cs.last_press_ms) > click_window_ms_) {
    fire_click_(cs.count, is_up);
    cs.count = 0;
  }
}

void ShutterGroup::fire_click_(uint8_t count, bool is_up) {
  if (count == 2) {
    Dir d = is_up ? Dir::UP : Dir::DOWN;
    // A manual (wall-switch) command cancels this group's sun automation for
    // the day. Set the group flags here, at the manual-command origin, then
    // enqueue each member as a wall-switch command so the hub preempts.
    if (d == Dir::UP) {
      this->sunrise_cancelled_today     = true;
      this->sun_through_cancelled_today = true;
      this->closed_by_sun_through       = false;
    } else {
      this->sunset_cancelled_today = true;
    }
    for (auto *s : shutters_) if (hub_ != nullptr) hub_->enqueue(s, d, true);
  } else if (count == 3) {
    if (hub_ != nullptr) hub_->cancel_group(this);
  }
}

void ShutterGroup::publish_sun_through(bool on) {
  if (sun_through_sensor_ != nullptr) sun_through_sensor_->publish_state(on);
}

bool ShutterGroup::compute_sun_through_raw(float az, float el) const {
  // NaN guard intentionally omitted: the sole caller,
  // ShutterHub::sun_through_active(), already rejects NaN az/el before
  // calling here, so a guard here would be provably redundant.
  if (!has_window_) return false;
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
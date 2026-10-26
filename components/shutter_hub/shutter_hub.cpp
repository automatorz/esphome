#include "shutter_hub.h"
#include "shutter.h"
#include "shutter_group.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/application.h"
#include "esphome/core/time.h"
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
static constexpr uint16_t POSITIONS_VERSION = 1;
static constexpr uint32_t POSITIONS_PREF_HASH = 0x5A00F00D;

void ShutterHub::setup() {
  ESP_LOGCONFIG(TAG, "Shutter hub ready: %u shutters, %u groups",
                (unsigned) shutters_.size(), (unsigned) groups_.size());

  // Re-evaluate sun-through whenever either sun sensor publishes a new value.
  // This internalizes what used to be two YAML `sensor: platform: sun`
  // `on_value: lambda: id(shutterhub).on_sun_update();` triggers, so the user
  // no longer needs those lambdas. Event-driven off the sun sensor updates
  // (no polling added). Guarded so a missing sensor is a no-op.
  if (this->sun_az_ != nullptr)
    this->sun_az_->add_on_state_callback([this](float) { this->on_sun_update(); });
  if (this->sun_el_ != nullptr)
    this->sun_el_->add_on_state_callback([this](float) { this->on_sun_update(); });
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
    positions_block_.version = POSITIONS_VERSION;
    positions_block_.count = MAX_SHUTTER_POSITIONS;
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
  if (loaded.magic == POSITIONS_MAGIC &&
      loaded.version == POSITIONS_VERSION &&
      loaded.count == MAX_SHUTTER_POSITIONS) {
    memcpy(&positions_block_, &loaded, sizeof(positions_block_));
    ESP_LOGI(TAG, "RESTORE: loaded positions block from NVS");
    for (size_t i = 0; i < shutters_.size() && i < MAX_SHUTTER_POSITIONS; i++) {
      ESP_LOGI(TAG, "RESTORE:   shutter[%u] = %.3f",
               (unsigned) i, positions_block_.positions[i]);
    }
  } else {
    // Magic/version/count mismatch: either a never-written block or an
    // incompatible older layout. A one-time reset of saved positions is
    // intended here -- start fresh (positions_block_ already holds the
    // freshly-initialized NaN defaults set above).
    ESP_LOGW(TAG,
             "RESTORE: block rejected (magic=0x%08X ver=%u count=%u, "
             "expected magic=0x%08X ver=%u count=%u), starting fresh",
             (unsigned) loaded.magic, (unsigned) loaded.version,
             (unsigned) loaded.count, (unsigned) POSITIONS_MAGIC,
             (unsigned) POSITIONS_VERSION, (unsigned) MAX_SHUTTER_POSITIONS);
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
  const uint32_t now = millis();

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
        for (auto *g : groups_) {
          if (!g->close_at_sunset() || g->sunset_cancelled_today) continue;
          for (auto *s : g->shutters()) {
            if (s->position_known && s->position <= 0.01f) continue;
            this->enqueue(s, Dir::DOWN, false);
          }
        }
      }
    }
  }

  // Daily housekeeping + scheduled backup open. Clock check only, no I2C.
  if (time_ != nullptr) {
    auto t = time_->now();
    if (t.is_valid()) {
      // Day rollover: reset the per-group daily-cancel flags AND re-arm the
      // backup-open latch, exactly once when a new calendar day begins. This is
      // internal now — no external midnight on_time trigger is required. The
      // latch re-arm lives HERE (not in reset_daily_flags()) so it only happens
      // at the true rollover, where the clock is at ~00:00 (before
      // backup_open_time) and the backup open therefore waits for the configured
      // time rather than firing immediately. The manual "Clear Overrides" button
      // also calls reset_daily_flags() but must NOT re-arm the latch (it can be
      // pressed after backup_open_time, which would fire the open retroactively).
      // Guarded so it runs once per day.
      if ((int16_t) t.day_of_year != reset_last_day_) {
        const bool first_sync = (reset_last_day_ == -1);
        reset_last_day_ = (int16_t) t.day_of_year;
        this->reset_daily_flags();
        // Re-arm the backup-open latch for the new day. EXCEPT on the very first
        // clock sync after boot: if the device boots/syncs AFTER backup_open_time
        // (e.g. a reboot in the evening), arming it would make the backup open
        // fire retroactively and open shutters at night. On first sync, only arm
        // if we are still BEFORE the configured time today; otherwise treat it as
        // already fired for today. True midnight rollovers always arm (00:00 is
        // before any sane backup_open_time).
        if (first_sync && backup_open_hour_ != 255) {
          uint16_t now_minutes = (uint16_t) t.hour * 60 + t.minute;
          uint16_t target_minutes =
              (uint16_t) backup_open_hour_ * 60 + backup_open_minute_;
          this->backup_open_fired_today_ = (now_minutes >= target_minutes);
        } else {
          this->backup_open_fired_today_ = false;
        }
      }

      // Feature 1: scheduled backup open. Inert until set_backup_open_time() is
      // called (hour stays 255). Fires at most once per day via the latch that
      // reset_daily_flags() re-armed at the day rollover above.
      if (backup_open_hour_ != 255 && !backup_open_fired_today_) {
        uint16_t now_minutes = (uint16_t) t.hour * 60 + t.minute;
        uint16_t target_minutes =
            (uint16_t) backup_open_hour_ * 60 + backup_open_minute_;
        if (now_minutes >= target_minutes) {
          this->maybe_backup_open_();
          backup_open_fired_today_ = true;
        }
      }
    }
  }

  switch (state_) {
    case HubState::IDLE:
      if (!queue_.empty()) this->begin_request_();
      break;

    case HubState::SETTLING:
      if ((uint32_t)(now - state_start_ms_) >= SETTLE_MS) {
        state_ = HubState::MOVING;
        state_start_ms_ = now;
        last_pos_publish_ms_ = now - POS_PUBLISH_INTERVAL_MS;
        last_group_update_pos_ = active_start_pos_;
        if (active_ != nullptr) active_->publish_state(false);
      }
      break;

    case HubState::MOVING:
      if ((uint32_t)(now - state_start_ms_) >= active_duration_ms_) {
        this->complete_active_();
      } else {
        this->refresh_active_position_();
      }
      break;

    case HubState::COOLDOWN:
      if ((uint32_t)(now - state_start_ms_) >= INTER_REQUEST_DELAY_MS) {
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
  // CRASH BREADCRUMB (low volume: once per movement start). We've seen a rare
  // (~weekly) StoreProhibited null write; this records which shutter is about to
  // be driven RIGHT BEFORE the dereferences below, so if the crash is here the
  // last log line names the shutter (and an active_==nullptr here would be the
  // smoking gun — a null Request.shutter). Deliberately NOT guarded: the user
  // wants it to crash so the esp32.crash backtrace + uptime reset are visible.
  ESP_LOGD(TAG, "begin_request_: active_=%p dir=%s queue_remaining=%u",
           (void *) active_, active_dir_ == Dir::UP ? "UP" : "DOWN",
           (unsigned) queue_.size());
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
  const uint32_t now = millis();
  uint32_t elapsed = now - state_start_ms_;
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
  // NOTE: enqueue no longer touches any daily-cancel flags. Those flags live
  // on ShutterGroup and are set by the group at the manual-command origin
  // (ShutterGroup::fire_click_ / control). from_wall_switch here only controls
  // queue preemption, not cancel semantics.
  queue_.erase(
      std::remove_if(queue_.begin(), queue_.end(),
                     [s](const Request &r) { return r.shutter == s; }),
      queue_.end());

  // Skip a command for a shutter already at the commanded limit, mirroring the
  // sun-automation paths (which skip members with position >= 0.99 for UP /
  // <= 0.01 for DOWN). Without this, a double-click UP on a fully-open shutter
  // (or DOWN on a fully-closed one) fired a pointless ~50ms relay pulse via the
  // minimum-duration clamp in begin_request_(). We only skip when the shutter
  // is NOT the active mover: a mid-travel reverse (active_ == s, opposite dir)
  // is at an intermediate position, not at the target limit, so this check is
  // naturally false for it and the reverse still works. position_known guard:
  // if we don't actually know the position, don't skip (let it run so the user
  // can still command it).
  if (active_ != s && s->position_known) {
    if (d == Dir::UP && s->position >= 0.99f) {
      return;
    }
    if (d == Dir::DOWN && s->position <= 0.01f) {
      return;
    }
  }

  if (active_ == s) {
    if (active_dir_ == d) return;
    stop_active();
    // A mid-travel reverse stops the active mover and immediately enqueues the
    // opposite-direction request below. finalize_active_() (called by
    // stop_active()) picked IDLE because the queue was still empty at that
    // instant, which would let the reverse start with no quiet gap. Force
    // COOLDOWN here so the full INTER_REQUEST_DELAY_MS elapses before the
    // reversing relay is driven (UX + I2C-crash mitigation). The genuine
    // empty-queue fast-IDLE path (travel finished, plain stop) does not go
    // through enqueue() and is unaffected.
    state_ = HubState::COOLDOWN;
    state_start_ms_ = millis();
  } else if (active_ != nullptr && from_wall_switch) {
    Request preempted{active_, active_dir_, false};
    stop_active();  // resets preempted shutter's icon to IDLE
    // Same reasoning as the reverse path: preempting a DIFFERENT moving group
    // from a wall switch stops the active mover and enqueues new work in this
    // same call, so honor the quiet inter-request window before the next relay.
    state_ = HubState::COOLDOWN;
    state_start_ms_ = millis();
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
  if (from_wall_switch) queue_.push_front({s, d, true});
  else                  queue_.push_back({s, d, false});

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
    // Snapshot the position at the moment of the stop. Use the SAME
    // interpolation as refresh_active_position_(): position =
    // start + (target - start) * frac, where frac is the fraction of THIS
    // move completed. Do NOT add/subtract frac to the running position --
    // refresh_active_position_() has already advanced active_->position
    // toward the target, so re-applying frac double-counts the travel and
    // (e.g. closing from fully open, stopped halfway) snaps it to 0/closed.
    uint32_t elapsed = millis() - state_start_ms_;
    if (elapsed > active_duration_ms_) elapsed = active_duration_ms_;
    float frac = active_duration_ms_ == 0 ? 1.0f
                : (float) elapsed / (float) active_duration_ms_;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    float target = (active_dir_ == Dir::UP) ? 1.0f : 0.0f;
    float new_pos = active_start_pos_ + (target - active_start_pos_) * frac;
    if (new_pos < 0.0f) new_pos = 0.0f;
    if (new_pos > 1.0f) new_pos = 1.0f;
    active_->position = new_pos;
    active_->position_known = true;
  }
  this->finalize_active_(active_);
}

// Shared tail of stop_active() and complete_active_(): runs AFTER the final
// position has been set on the finishing shutter. Marks it idle, releases the
// relays, publishes/saves, clears the active slot, and arms the quiet
// inter-request window (COOLDOWN) unless the queue is empty. Behavior and
// timing are identical to the inlined versions it replaced.
void ShutterHub::finalize_active_(Shutter *finished) {
  if (finished == nullptr) return;
  finished->current_operation = cover::COVER_OPERATION_IDLE;
  finished->set_queued(false, Dir::NONE);
  finished->de_energize();
  finished->publish_state_from_position();
  finished->save_position();
  active_ = nullptr;
  active_dir_ = Dir::NONE;

  // If there's still work queued, give the bus a quiet window before
  // starting the next one. If the queue is empty, go straight to IDLE
  // so a fresh user command responds immediately.
  state_start_ms_ = millis();
  state_ = queue_.empty() ? HubState::IDLE : HubState::COOLDOWN;

  this->update_group_states_for(finished);
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
  this->finalize_active_(active_);
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

bool ShutterHub::sun_through_active(ShutterGroup *g) {
  if (!g->has_window()) return false;
  float az = this->sun_azimuth();
  float el = this->sun_elevation();
  if (std::isnan(az) || std::isnan(el)) return false;
  if (el < g->elevation_margin()) return false;
  return g->compute_sun_through_raw(az, el);
}

void ShutterHub::maybe_backup_open_() {
  // Backup open: for groups that close at sunset but do NOT open at sunrise,
  // open any member still closed. Skip the whole group if it was already
  // opened/cancelled today or is currently closed by sun-through. Movement is
  // one shutter at a time through the hub queue, mirroring schedule_sunrise_open.
  ESP_LOGI(TAG, "Backup open: checking groups at %02u:%02u",
           (unsigned) backup_open_hour_, (unsigned) backup_open_minute_);
  for (auto *g : groups_) {
    if (!g->close_at_sunset() || g->open_at_sunrise()) continue;
    if (g->sunrise_cancelled_today || g->closed_by_sun_through) continue;
    for (auto *s : g->shutters()) {
      // Only open members that are currently closed; skip the rest.
      if (!(s->position_known && s->position <= 0.01f)) continue;
      this->enqueue(s, Dir::UP, false);
    }
    // Treat the backup open as the morning open having happened for this group.
    g->sunrise_cancelled_today = true;
  }
}

void ShutterHub::open_all_closed_no_override() {
  // Open every currently-closed shutter across all groups EXCEPT groups closed
  // by sun-through or already opened/cancelled today. One shutter at a time
  // through the hub queue; never drives relays directly.
  ESP_LOGI(TAG, "Open-all: opening all closed shutters (no override)");
  for (auto *g : groups_) {
    if (g->closed_by_sun_through) continue;
    if (g->sunrise_cancelled_today) continue;
    for (auto *s : g->shutters()) {
      if (!(s->position_known && s->position <= 0.01f)) continue;
      this->enqueue(s, Dir::UP, false);
    }
  }
}

void ShutterHub::schedule_sunrise_open() {
  for (auto *g : groups_) {
    if (!g->open_at_sunrise() || g->sunrise_cancelled_today) continue;
    if (g->suppress_sunrise_if_sun_through() && this->sun_through_active(g)) continue;
    uint32_t off = g->sunrise_offset_ms();
    for (auto *s : g->shutters()) {
      // Option (b): skip any member already open; enqueue the rest one at a
      // time through the existing hub queue.
      if (s->position_known && s->position >= 0.99f) continue;
      if (off == 0) this->enqueue(s, Dir::UP, false);
      else this->set_timeout(off, [this, s]() { this->enqueue(s, Dir::UP, false); });
    }
  }
}

void ShutterHub::schedule_sunset_close() {
  for (auto *g : groups_) {
    if (!g->close_at_sunset() || g->sunset_cancelled_today) continue;
    uint32_t off = g->sunset_offset_ms();
    for (auto *s : g->shutters()) {
      // Option (b): skip any member already closed.
      if (s->position_known && s->position <= 0.01f) continue;
      if (off == 0) this->enqueue(s, Dir::DOWN, false);
      else this->set_timeout(off, [this, s]() { this->enqueue(s, Dir::DOWN, false); });
    }
  }
}

void ShutterHub::on_sun_update() {
  for (auto *g : groups_) {
    if (!g->has_window()) continue;
    bool active = this->sun_through_active(g);
    g->publish_sun_through(active);
    if (!g->close_on_sun_through()) continue;
    // For the cancelled / heating paths we take no sun-through ACTION, but we
    // still track the edge so that when the suppression clears, the NEXT update
    // does not see a stale prev-active and fire a spurious reopen.
    if (g->sun_through_cancelled_today || this->heating_needed_on()) {
      g->sun_through_prev_active_ = active;
      continue;
    }
    if (active) {
      // Sun is shining through: close any member not already closed. Only set
      // closed_by_sun_through when we ACTUALLY enqueued a close — previously it
      // was set unconditionally, so a shutter already closed from the prior
      // sunset got falsely flagged as "closed by sun-through". With the reopen
      // logic below no longer depending on that flag this is just hygiene, but
      // it keeps the flag honest for telemetry/other logic.
      bool closed_any = false;
      for (auto *s : g->shutters()) {
        if (s->position_known && s->position <= 0.01f) continue;  // already closed
        this->enqueue(s, Dir::DOWN, false);
        closed_any = true;
      }
      if (closed_any)
        g->closed_by_sun_through = true;
    } else {
      // Sun-through has ended. Reopen if this is a daylight-open group
      // (open_at_sunrise), there is still meaningful daylight left
      // (el > elevation_margin), and the user has not manually overridden the
      // group today. This is INDEPENDENT of closed_by_sun_through and of
      // close_at_sunset: it performs the daytime open that may have been
      // suppressed at sunrise (suppress_sunrise_if_sun_through) and never
      // retried, OR reverses a sun-through close. The elevation gate naturally
      // lets an EAST window reopen (sun still high when its morning sun-through
      // ends) while preventing a pointless dusk reopen on a WEST window (sun
      // already low when its afternoon sun-through ends, just before the
      // sunset close).
      float el = this->sun_elevation();
      // EDGE GATE: only reopen on a genuine active->inactive transition
      // (sun_through_prev_active_ was true), NOT on any standalone inactive
      // reading. This is what fixes the reboot glitch: at boot (or while the sun
      // sensors are still settling, az/el briefly NaN -> active=false) there is
      // no prior active state, so no edge, so a sun-through-closed shutter is
      // NOT spuriously opened-then-closed. It also matches the real intent:
      // reopen WHEN the sun stops shining through, not merely whenever it isn't.
      //
      // is_overridden() covers ANY manual command today (manual open/close or
      // sun_through cancel), so a shutter the user deliberately closed mid-day
      // is not reopened. The elevation gate lets EAST reopen at midday while
      // preventing a pointless WEST dusk reopen.
      if (g->sun_through_prev_active_ && g->open_at_sunrise() &&
          !g->is_overridden() && !std::isnan(el) && el > g->elevation_margin()) {
        for (auto *s : g->shutters()) {
          if (s->position_known && s->position >= 0.99f) continue;  // already open
          this->enqueue(s, Dir::UP, false);
        }
      }
      g->closed_by_sun_through = false;
    }
    // Record this reading for the next update's edge detection.
    g->sun_through_prev_active_ = active;
  }
}

void ShutterHub::reset_daily_flags() {
  // Clears the per-group manual-override / sun-through daily flags and
  // republishes the override sensors. Called in TWO situations:
  //   1. the internal midnight day-rollover (loop()), and
  //   2. the manual "Clear Shutter Overrides" button, which can be pressed at
  //      ANY time of day.
  // It deliberately does NOT re-arm the backup-open latch
  // (backup_open_fired_today_). Re-arming is done ONLY by the day-rollover in
  // loop(), where it is safe because the clock is at 00:00 (before the configured
  // backup_open_time). Re-arming here would let the button retroactively trigger
  // the backup open: pressing it in the evening (now >= backup_open_time) would
  // immediately open every close-at-sunset/not-open-at-sunrise shutter — the bug
  // observed when the button opened shutters at night.
  for (auto *g : groups_) {
    g->sunrise_cancelled_today     = false;
    g->sunset_cancelled_today      = false;
    g->sun_through_cancelled_today = false;
    g->closed_by_sun_through       = false;
    g->publish_overridden();
  }
  ESP_LOGD(TAG, "Daily flags reset");
}

}}  // namespace shutter_hub
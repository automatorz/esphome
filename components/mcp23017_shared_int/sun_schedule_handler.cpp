#include "sun_schedule_handler.h"

#include "esphome/components/shutter_hub/shutter_hub.h"
#include "esphome/core/time.h"

namespace esphome {
namespace mcp23017_shared_int {

static const char *const PLACEHOLDER = "--:--:--";

void SunScheduleHandler::on_sunset() {
  for (const auto &e : this->sunset_) {
    if (e.turn_on) {
      e.light->turn_on().perform();
    } else {
      e.light->turn_off().perform();
    }
  }
  if (this->hub_ != nullptr) {
    this->hub_->schedule_sunset_close();
  }
}

void SunScheduleHandler::on_sunrise() {
  for (const auto &e : this->sunrise_) {
    if (e.turn_on) {
      e.light->turn_on().perform();
    } else {
      e.light->turn_off().perform();
    }
  }
  if (this->hub_ != nullptr) {
    this->hub_->schedule_sunrise_open();
  }
}

void SunScheduleHandler::publish_next_(bool rising, text_sensor::TextSensor *sensor, std::string *last) {
  if (sensor == nullptr)
    return;

  std::string value = PLACEHOLDER;
  if (this->sun_ != nullptr) {
    // sun::Sun::sunrise/sunset read the configured time; before the clock is
    // synced the result is empty, so we leave the placeholder in place.
    optional<ESPTime> event = rising ? this->sun_->sunrise(this->elevation_) : this->sun_->sunset(this->elevation_);
    if (event.has_value() && event->is_valid(false, false)) {
      char buf[16];
      event->strftime(buf, sizeof(buf), "%H:%M:%S");
      value = buf;
    }
  }

  // Dedup: only publish when the formatted value actually changes.
  if (value != *last) {
    *last = value;
    sensor->publish_state(value);
  }
}

void SunScheduleHandler::update() {
  this->publish_next_(false, this->next_sunset_sensor_, &this->last_sunset_);
  this->publish_next_(true, this->next_sunrise_sensor_, &this->last_sunrise_);
}

}  // namespace mcp23017_shared_int
}  // namespace esphome

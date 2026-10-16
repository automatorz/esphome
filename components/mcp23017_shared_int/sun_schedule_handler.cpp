#include "sun_schedule_handler.h"

#include "esphome/components/shutter_hub/shutter_hub.h"

namespace esphome {
namespace mcp23017_shared_int {

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

}  // namespace mcp23017_shared_int
}  // namespace esphome

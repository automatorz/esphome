#pragma once
#include "esphome/components/button/button.h"
#include "shutter_hub.h"

namespace esphome { namespace shutter_hub {

// Button entity that clears every group's daily "overridden" flags, i.e. a
// manual reset_daily_flags(). Pressing it only flips flags and publishes the
// per-group "overridden" binary_sensors (and re-arms the backup-open latch);
// it moves no shutters.
// It is a plain button::Button (EntityBase only, NOT a Component), so it is
// registered via App.register_button and does NOT count toward
// ESPHOME_COMPONENT_COUNT, exactly like OpenAllButton.
class ClearOverridesButton : public button::Button {
 public:
  void set_hub(ShutterHub *h) { hub_ = h; }

 protected:
  void press_action() override {
    if (hub_ != nullptr) hub_->reset_daily_flags();
  }

  ShutterHub *hub_{nullptr};
};

}}  // namespace shutter_hub

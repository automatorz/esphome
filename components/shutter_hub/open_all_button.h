#pragma once
#include "esphome/components/button/button.h"
#include "shutter_hub.h"

namespace esphome { namespace shutter_hub {

// Button entity that opens every currently-closed shutter across all groups,
// skipping groups closed by sun-through or already opened/cancelled today.
// It is a plain button::Button (EntityBase only, NOT a Component), so it is
// registered via App.register_button and does NOT count toward
// ESPHOME_COMPONENT_COUNT. The press delegates to the hub, which drives the
// movement one shutter at a time through its queue.
class OpenAllButton : public button::Button {
 public:
  void set_hub(ShutterHub *h) { hub_ = h; }

 protected:
  void press_action() override {
    if (hub_ != nullptr) hub_->open_all_closed_no_override();
  }

  ShutterHub *hub_{nullptr};
};

}}  // namespace shutter_hub

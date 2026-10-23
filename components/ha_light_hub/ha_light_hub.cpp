#include "ha_light_hub.h"
#include "esphome/core/log.h"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>

// NIT3: codegen force-defines USE_API_HOMEASSISTANT_SERVICES (see
// ha_light_hub/__init__.py), which compiles the HA-services code paths into the
// shared `api` component for this build (a small flash cost, no behavioral
// change for other components). The user-visible equivalent is
// `api: { homeassistant_services: true }`. We guard the api includes AND the two
// send bodies with the EXACT dual condition that wraps homeassistant_service.h
// and APIServer::send_homeassistant_action, so if either define is ever absent
// the send becomes a genuine no-op rather than referencing a compiled-out class.
#if defined(USE_API) && defined(USE_API_HOMEASSISTANT_SERVICES)
#include "esphome/components/api/api_server.h"
#include "esphome/components/api/api_pb2.h"
#endif

namespace esphome {
namespace ha_light_hub {

static const char *const TAG = "ha_light_hub";

uint8_t HaLightDimmer::next_level_(int cur, const std::vector<uint8_t> &levels) {
  // 1. Find the configured level the light is currently "at" (within
  //    LEVEL_MATCH_TOL %). Scan ascending; the FIRST level within tolerance wins.
  int matched = -1;  // index into levels
  for (size_t i = 0; i < levels.size(); i++) {
    if (abs((int) levels[i] - cur) <= LEVEL_MATCH_TOL) {
      matched = (int) i;
      break;
    }
  }
  // 2a. Matched a configured level -> advance to the next; clamp at the top.
  if (matched >= 0)
    return (matched + 1 < (int) levels.size()) ? levels[matched + 1] : levels[matched];
  // 2b. No match (e.g. off at 0, or an intermediate value) -> first level
  //     strictly > cur, else clamp to max.
  for (uint8_t lv : levels) {
    if ((int) lv > cur)
      return lv;
  }
  return levels.back();
}

int HaLightDimmer::current_pct_() {
  // Finding 1: during the trust window our own last command is authoritative,
  // because the HA sensor cache lags by a round-trip. After PENDING_TTL_MS we
  // fall back to the cached sensor read so external changes win.
  uint32_t now = millis();
  if (this->pending_pct_ >= 0 && (uint32_t) (now - this->pending_set_ms_) < PENDING_TTL_MS)
    return this->pending_pct_;

  if (this->state_ == nullptr || this->brightness_ == nullptr || this->state_->state != "on")
    return 0;  // "off"/unknown -> 0
  float b = this->brightness_->state;  // HA brightness 0..255
  if (std::isnan(b))
    return 0;
  return (int) lroundf(b / 255.0f * 100.0f);
}

void HaLightDimmer::on_up_pressed() {
  uint32_t now = millis();
  if (this->debounce_ms_ != 0 && (uint32_t) (now - this->last_up_ms_) < this->debounce_ms_)
    return;  // drop bounce edge
  this->last_up_ms_ = now;
  this->call_turn_on_(next_level_(this->current_pct_(), this->levels_));
}

void HaLightDimmer::on_down_pressed() {
  uint32_t now = millis();
  // Debounce gate also serves as the post-double-click refractory: last_down_ms_
  // is stamped on every ACCEPTED edge, so a bounce within debounce_ms_ of the
  // double is dropped before it can arm a spurious single-click turn_off.
  if (this->debounce_ms_ != 0 && (uint32_t) (now - this->last_down_ms_) < this->debounce_ms_)
    return;  // drop bounce edge
  this->last_down_ms_ = now;

  if (this->down_pending_) {  // 2nd accepted press within window -> DOUBLE
    this->cancel_timeout("down");
    this->down_pending_ = false;
    this->fire_down_double_();
  } else {  // 1st accepted press -> arm single-click timer
    this->down_pending_ = true;
    // NIT4: "down" is a string literal (static storage), as required by
    // Component::set_timeout(const char *). Do NOT convert it to a per-instance
    // std::string whose .c_str() would violate the static-lifetime contract.
    this->set_timeout("down", this->dbl_window_ms_, [this]() {
      this->down_pending_ = false;
      this->fire_down_single_();
    });
  }
}

void HaLightDimmer::fire_down_single_() { this->call_turn_off_(); }

void HaLightDimmer::fire_down_double_() {
  this->call_turn_on_(this->levels_.front());  // min level (levels_ is ascending)
}

void HaLightDimmer::call_turn_on_(uint8_t pct) {
  // Finding 1: stamp the pending level BEFORE sending so a rapid second
  // up-press reads the just-commanded level. Done unconditionally (outside the
  // API guard) so optimistic tracking works even if the send compiles to nothing.
  this->pending_pct_ = pct;
  this->pending_set_ms_ = millis();
#if defined(USE_API) && defined(USE_API_HOMEASSISTANT_SERVICES)
  if (api::global_api_server == nullptr)
    return;
  // ESPHome 2026.10.0 replaced the runtime set_service/init_data/add_data
  // interface of HomeAssistantServiceCallAction with a codegen-built static
  // fields table, which does not fit a dynamically-built call. Build the
  // HomeassistantActionRequest ourselves and send it directly — exactly what
  // the action's play() does internally. NOTE: HomeassistantActionRequest and
  // HomeassistantServiceMap use non-owning StringRef, so the backing std::string
  // buffers (svc, entity, kv values) MUST outlive the synchronous
  // send_homeassistant_action() call below — they are function-scope locals, so
  // they do.
  std::string svc = "light.turn_on";
  std::string entity = this->entity_id_;
  std::string pct_val = std::to_string((unsigned) pct);
  char tr_buf[16];
  snprintf(tr_buf, sizeof(tr_buf), "%g", this->transition_s_);  // "0.5"
  std::string tr_val = tr_buf;
  std::string k_entity = "entity_id", k_pct = "brightness_pct", k_tr = "transition";

  api::HomeassistantActionRequest req;
  req.service = esphome::StringRef(svc);
  req.is_event = false;
  req.data.init(3);
  { auto &kv = req.data.emplace_back(); kv.key = esphome::StringRef(k_entity); kv.value = esphome::StringRef(entity); }
  { auto &kv = req.data.emplace_back(); kv.key = esphome::StringRef(k_pct);    kv.value = esphome::StringRef(pct_val); }
  { auto &kv = req.data.emplace_back(); kv.key = esphome::StringRef(k_tr);     kv.value = esphome::StringRef(tr_val); }
  api::global_api_server->send_homeassistant_action(req);
#endif
}

void HaLightDimmer::call_turn_off_() {
  // Finding 1: off is level 0; stamp it before sending (see call_turn_on_).
  this->pending_pct_ = 0;
  this->pending_set_ms_ = millis();
#if defined(USE_API) && defined(USE_API_HOMEASSISTANT_SERVICES)
  if (api::global_api_server == nullptr)
    return;
  // See call_turn_on_ for the 2026.10.0 direct-request rationale and the
  // StringRef lifetime requirement (all backing strings are function-scope).
  std::string svc = "light.turn_off";
  std::string entity = this->entity_id_;
  char tr_buf[16];
  snprintf(tr_buf, sizeof(tr_buf), "%g", this->transition_s_);
  std::string tr_val = tr_buf;
  std::string k_entity = "entity_id", k_tr = "transition";

  api::HomeassistantActionRequest req;
  req.service = esphome::StringRef(svc);
  req.is_event = false;
  req.data.init(2);
  { auto &kv = req.data.emplace_back(); kv.key = esphome::StringRef(k_entity); kv.value = esphome::StringRef(entity); }
  { auto &kv = req.data.emplace_back(); kv.key = esphome::StringRef(k_tr);     kv.value = esphome::StringRef(tr_val); }
  api::global_api_server->send_homeassistant_action(req);
#endif
}

void HaLightDimmer::dump_config() {
  ESP_LOGCONFIG(TAG, "HA Light Dimmer:");
  ESP_LOGCONFIG(TAG, "  Entity: %s", this->entity_id_);
  std::string levels;
  for (size_t i = 0; i < this->levels_.size(); i++) {
    if (i)
      levels += ", ";
    char b[8];
    snprintf(b, sizeof(b), "%u", (unsigned) this->levels_[i]);
    levels += b;
  }
  ESP_LOGCONFIG(TAG, "  Levels (%%): [%s]", levels.c_str());
  ESP_LOGCONFIG(TAG, "  Transition: %g s", this->transition_s_);
  ESP_LOGCONFIG(TAG, "  Double-click window: %u ms", (unsigned) this->dbl_window_ms_);
  ESP_LOGCONFIG(TAG, "  Debounce: %u ms", (unsigned) this->debounce_ms_);
}

}  // namespace ha_light_hub
}  // namespace esphome

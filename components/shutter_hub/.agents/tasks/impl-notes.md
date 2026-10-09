# shutter_hub telemetry implementation notes

Additive, default-ON, opt-out per-shutter status + position telemetry for Home
Assistant. No compiler/toolchain exists in this workspace, so this was verified
by self-review only. The user must build on-device to confirm compilation.

## Per-file changes

### `__init__.py` (Python codegen + schema)
- Added `text_sensor` to the `esphome.components` import; added `text_sensor`
  and `sensor` to `AUTO_LOAD` (kept existing `cover`, `binary_sensor`).
- Imported `CORE, ID` from `esphome.core` and `sanitize, snake_case` from
  `esphome.helpers`.
- Declared two new config constants: `CONF_STATUS_SENSOR = "status_sensor"`,
  `CONF_POSITION_SENSOR = "position_sensor"`.
- Extended `SHUTTER_SCHEMA` with two optional booleans, both `default=True`:
  `status_sensor`, `position_sensor`.
- Added `_declare_entity_id(obj_id, cls)` helper that mints a unique, sanitized
  declaration `ID` for entities synthesized after validation (same reasoning as
  the sibling `mcp23017_shared_int` component, which also builds IDs explicitly
  because `cv.GenerateID()` only runs during validation). It de-dupes against
  `CORE.has_id`.
- In the per-shutter loop in `to_code`, after `cover.new_cover`, when the flag
  is set it builds a `text_sensor.text_sensor_schema(icon=...)` /
  `sensor.sensor_schema(unit_of_measurement="%", accuracy_decimals=0, icon=...)`
  config (explicit `CONF_ID` + derived `CONF_NAME`), creates the entity via
  `text_sensor.new_text_sensor` / `sensor.new_sensor`, and wires it with
  `sv.set_status_sensor(...)` / `sv.set_position_sensor(...)`.
  Default name is `"<Shutter Name> Status"` / `"<Shutter Name> Position"`,
  derived from the cover config `CONF_NAME` (falls back to the id if absent).
- Because both default True, every existing shutter gains both entities on the
  next build — the intended, user-confirmed opt-out/default-on behavior.

### `shutter.h`
- Added includes: `text_sensor/text_sensor.h`, `sensor/sensor.h`.
- Added setters `set_status_sensor(text_sensor::TextSensor*)` and
  `set_position_sensor(sensor::Sensor*)` — names/types match the Python wiring
  exactly.
- Added `set_queued(bool q, Dir d)` and declarations `publish_status()` and
  `publish_position_sensor()`.
- Added members: `text_sensor::TextSensor *status_sensor_{nullptr}`,
  `sensor::Sensor *position_sensor_{nullptr}`, `bool queued_{false}`,
  `Dir queued_dir_{Dir::NONE}`.

### `shutter.cpp`
- Added `#include <cstdio>` (for `snprintf`).
- `publish_state_from_position()` now also calls `publish_status()` and
  `publish_position_sensor()`. This covers restore (`apply_loaded_position()`
  already calls it) and every stop/complete path.
- Implemented `publish_status()`: null-guarded no-op if `status_sensor_` is
  null. Resolves active-vs-queued via `hub_->active_shutter() == this` plus the
  `queued_` flag, consistent with the existing queued-icon logic.
- Implemented `publish_position_sensor()`: null-guarded no-op; publishes
  `round(clamp(position,0,1)*100)`.

### `shutter_hub.cpp`
- `enqueue()`: when a non-active shutter is queued -> `set_queued(true, d)` +
  `publish_status()`. In the wall-switch preemption branch, the re-queued
  preempted shutter gets `set_queued(true, preempted.dir)` + `publish_status()`.
- `begin_request_()`: active shutter gets `set_queued(false, Dir::NONE)` and
  `publish_status()` after `current_operation` is set.
- `cancel_shutter()`: previously-queued (now cancelled) shutter gets
  `set_queued(false, Dir::NONE)` before republishing.
- `stop_active()` / `complete_active_()`: finished shutter gets
  `set_queued(false, Dir::NONE)`; `publish_state_from_position()` there already
  republishes status + position via the edit above.
- `refresh_active_position_()`: inside the EXISTING ~750ms
  `POS_PUBLISH_INTERVAL_MS` throttle block, after `active_->publish_state(false)`
  it now also calls `active_->publish_status()` and
  `active_->publish_position_sensor()`. No new timer added.

### `shutter_group.{h,cpp}`
- Untouched. No group-level status sensor (user declined).

## Status-string vocabulary (as implemented)
- `"opening"`      — active mover, `current_operation == OPENING`
- `"closing"`      — active mover, `current_operation == CLOSING`
- `"queued_open"`  — `queued_` set and `queued_dir_ == UP`, not the active mover
- `"queued_close"` — `queued_` set and `queued_dir_ == DOWN`, not the active mover
- `"open"`         — idle, position >= 0.99
- `"closed"`       — idle, position <= 0.01
- `"partial:NN"`   — idle, strictly between; NN = `round(position*100)` (e.g.
  position 0.45 -> `"partial:45"`)

Position sensor: numeric percent `round(clamp(position,0,1)*100)`, unit "%",
0 decimals, icon `mdi:window-shutter`, no device_class.

## Safety / crash-path confirmation
- Every new publish call is null-guarded: `status_sensor: false` leaves
  `status_sensor_` null and all `publish_status()` calls are no-ops; same for
  `position_sensor_`.
- No edit touched `energize()`, `de_energize()`, the relay switch-write logic,
  the 50ms `RELAY_GAP_MS` set_timeout, the hub state-machine timings
  (`SETTLE_MS` / MOVING / `INTER_REQUEST_DELAY_MS` / COOLDOWN), the I2C /
  MCP23017 interaction, or anything in `mcp23017_shared_int`. The telemetry
  reuses existing publish points only; no new timers were introduced.
- Cover traits, position semantics, and queue logic are unchanged apart from the
  additive `queued_` / `queued_dir_` bookkeeping.

## Verification limitation
No ESPHome build toolchain is available in this workspace, so compilation could
NOT be performed. Changes were verified by careful self-review: Python schema
idioms (schema helpers, `cg.add`, `await`, `AUTO_LOAD`), exact name/type match
between the generated C++ setters and the header signatures, and null-guards on
all new publish calls. The user must build on-device to confirm.

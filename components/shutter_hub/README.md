# shutter_hub

ESPHome external component that drives roller shutters through relay switches,
one movement at a time via a shared hub queue. It aggregates shutters into
groups (with optional wall-switch click handling) and runs sun-based automation
(open at sunrise, close at sunset, close on sun-through) using the `sun`
component's azimuth/elevation sensors.

Entities created:

- A `cover` per shutter and per group.
- Optional per-shutter status (`text_sensor`) and position (`sensor`) telemetry.

## Breaking change: sun config moved from shutter to group

Sun-based automation and the window geometry it depends on now live on the
**group**, not on individual shutters. Each group owns ONE window and ONE
sun-through sensor.

Keys that moved from `shutters:` entries to `groups:` entries:

- `open_at_sunrise`
- `close_at_sunset`
- `sunrise_offset`
- `sunset_offset`
- `suppress_sunrise_if_sun_through`
- `close_on_sun_through`
- `window:` (the `width`/`depth`/`height`/`orientation`/`elevation_margin`
  block)
- `sun_through_sensor`

Keys that STAY on the shutter: `up_relay`, `down_relay`, `travel_time`,
`restore`, `device_class`, `name`, and the status/position telemetry toggles.

New group-level defaults make an unconfigured group do nothing at
sunrise/sunset/sun-through (matching the old "unset = no action" behavior):

- `open_at_sunrise` default `false`
- `close_at_sunset` default `false`
- `close_on_sun_through` default `false`
- `sunrise_offset` default `0min`
- `sunset_offset` default `0min`
- `suppress_sunrise_if_sun_through` default `true`
- `window` optional
- `sun_through_sensor` optional

At sunrise/sunset/sun-through the group enqueues each member shutter
individually through the hub queue, skipping any member already at the target
position. A manual command (wall-switch double-click or an HA command on the
group) cancels that group's sun automation for the rest of the day.

### Before (sun config on the shutter)

```yaml
shutter_hub:
  shutters:
    - id: kitchen_shutter_dev
      up_relay: relay_up
      down_relay: relay_down
      travel_time: 60s
      open_at_sunrise: true
      close_at_sunset: true
      sunrise_offset: 20min
      suppress_sunrise_if_sun_through: true
      close_on_sun_through: true
      sun_through_sensor: east_sun_through
      window:
        width: 1.9
        depth: 0.4
        height: 2.1
        orientation: EAST
        elevation_margin: 2.0
      restore: true
  groups:
    - id: kitchen_shutter
      shutters:
        - kitchen_shutter_dev
```

### After (sun config on the group)

```yaml
shutter_hub:
  shutters:
    - id: kitchen_shutter_dev
      up_relay: relay_up
      down_relay: relay_down
      travel_time: 60s
      restore: true
  groups:
    - id: kitchen_shutter
      shutters:
        - kitchen_shutter_dev
      open_at_sunrise: true
      close_at_sunset: true
      sunrise_offset: 20min
      suppress_sunrise_if_sun_through: true
      close_on_sun_through: true
      sun_through_sensor: east_sun_through
      window:
        width: 1.9
        depth: 0.4
        height: 2.1
        orientation: EAST
        elevation_margin: 2.0
```

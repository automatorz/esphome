import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import (
    cover, binary_sensor, sensor, text_sensor, sun, switch as switch_,
    time as time_, button,
)
from esphome.const import CONF_ID, CONF_NAME, CONF_DEVICE_CLASS, CONF_DEVICE_ID
from esphome.core import CORE, ID
from esphome.core.config import Device
from esphome.helpers import sanitize, snake_case

CODEOWNERS = ["@automatorz"]
DEPENDENCIES = ["sun"]
AUTO_LOAD = ["cover", "binary_sensor", "text_sensor", "sensor"]

ns = cg.esphome_ns.namespace("shutter_hub")
ShutterHub   = ns.class_("ShutterHub", cg.Component)
Shutter      = ns.class_("Shutter", cover.Cover, cg.Component)
ShutterGroup = ns.class_("ShutterGroup", cover.Cover, cg.Component)
OpenAllButton = ns.class_("OpenAllButton", button.Button)
ClearOverridesButton = ns.class_("ClearOverridesButton", button.Button)

WindowOrientation = ns.enum("WindowOrientation", True)
WINDOW_ORIENTATIONS = {"EAST": WindowOrientation.EAST, "WEST": WindowOrientation.WEST}

CONF_SHUTTERS            = "shutters"
CONF_GROUPS              = "groups"
CONF_UP_RELAY            = "up_relay"
CONF_DOWN_RELAY          = "down_relay"
CONF_TRAVEL_TIME         = "travel_time"
CONF_OPEN_AT_SUNRISE     = "open_at_sunrise"
CONF_CLOSE_AT_SUNSET     = "close_at_sunset"
CONF_SUNRISE_OFFSET      = "sunrise_offset"
CONF_SUNSET_OFFSET       = "sunset_offset"
CONF_SUPPRESS_SUNRISE    = "suppress_sunrise_if_sun_through"
CONF_WINDOW              = "window"
CONF_W                 = "width"
CONF_D                 = "depth"
CONF_H                 = "height"
CONF_ORIENT              = "orientation"
CONF_MARGIN              = "elevation_margin"
CONF_CLOSE_ON_SUN        = "close_on_sun_through"
CONF_SUN_THROUGH_SENSOR  = "sun_through_sensor"
CONF_SUN_AZ              = "sun_azimuth"
CONF_SUN_EL              = "sun_elevation"
CONF_HEATING             = "heating_needed"
CONF_WALL_UP             = "wall_up"
CONF_WALL_DOWN           = "wall_down"
CONF_CLICK_WIN           = "click_window"
CONF_DEBOUNCE            = "debounce"
CONF_RESTORE             = "restore"
CONF_TIME_ID             = "time_id"
CONF_STATUS_SENSOR       = "status_sensor"
CONF_POSITION_SENSOR     = "position_sensor"
CONF_BACKUP_OPEN_TIME    = "backup_open_time"
CONF_OPEN_ALL_BUTTON     = "open_all_button"
CONF_CLEAR_OVERRIDES_BUTTON = "clear_overrides_button"

DEFAULT_OPEN_ALL_BUTTON_NAME = "Open All Shutters"
DEFAULT_CLEAR_OVERRIDES_BUTTON_NAME = "Clear Shutter Overrides"


def _parse_hhmm(value):
    """Parse a plain "HH:MM" (seconds optional) time-of-day into (hour, minute).

    Only hour and minute are used by Feature 1; any seconds component is parsed
    for tolerance but ignored. Raises cv.Invalid on malformed input or ranges.
    """
    text = cv.string(value).strip()
    parts = text.split(":")
    if len(parts) not in (2, 3):
        raise cv.Invalid(
            f"'{CONF_BACKUP_OPEN_TIME}' must be HH:MM or HH:MM:SS, got '{text}'."
        )
    try:
        hour = int(parts[0])
        minute = int(parts[1])
        if len(parts) == 3:
            int(parts[2])  # validate seconds are numeric; value unused
    except ValueError:
        raise cv.Invalid(
            f"'{CONF_BACKUP_OPEN_TIME}' must be numeric HH:MM(:SS), got '{text}'."
        )
    if not (0 <= hour <= 23):
        raise cv.Invalid(f"'{CONF_BACKUP_OPEN_TIME}' hour must be 0..23, got {hour}.")
    if not (0 <= minute <= 59):
        raise cv.Invalid(
            f"'{CONF_BACKUP_OPEN_TIME}' minute must be 0..59, got {minute}."
        )
    return hour, minute

WINDOW_SCHEMA = cv.Schema({
    cv.Required(CONF_W): cv.positive_float,
    cv.Required(CONF_D): cv.positive_float,
    cv.Required(CONF_H): cv.positive_float,
    cv.Required(CONF_ORIENT): cv.enum(WINDOW_ORIENTATIONS, upper=True),
    cv.Optional(CONF_MARGIN, default=2.0): cv.float_,
})

# Both shutter and group default to device_class: shutter, so you can omit
# it entirely from your YAML.
SHUTTER_SCHEMA = cover.cover_schema(Shutter).extend({
    cv.Optional(CONF_DEVICE_CLASS, default="shutter"): cv.string,
    cv.Required(CONF_UP_RELAY): cv.use_id(switch_.Switch),
    cv.Required(CONF_DOWN_RELAY): cv.use_id(switch_.Switch),
    cv.Required(CONF_TRAVEL_TIME): cv.positive_time_period_milliseconds,
    cv.Optional(CONF_RESTORE, default=True): cv.boolean,
    # Telemetry entities exposed to Home Assistant. Both default on; set to
    # false per shutter to opt out of that entity.
    cv.Optional(CONF_STATUS_SENSOR, default=True): cv.boolean,
    cv.Optional(CONF_POSITION_SENSOR, default=True): cv.boolean,
})

# wall_up / wall_down are now optional. If both are omitted, the group
# still exists as a cover entity and can be commanded via HA, but it has
# no physical wall-switch control.
GROUP_SCHEMA = cover.cover_schema(ShutterGroup).extend({
    cv.Optional(CONF_DEVICE_CLASS, default="shutter"): cv.string,
    # wall_up / wall_down accept a binary_sensor reference. When shutter_hub is
    # nested in mcp23017_shared_int (the normal case), these reference the
    # board-generated input sensors ("has_board<bb>_chip<cc>_pin<pp>"), which are
    # fabricated at CODEGEN and so do NOT exist at validation time — a plain
    # cv.use_id would fail with "Couldn't find ID". We therefore accept a string
    # here and resolve it via the button_resolver passed into build_shutter_hub
    # (which looks the id up in the board-generated sensor map), mirroring how
    # up_relay/down_relay are resolved against the switch map. In the standalone
    # (non-nested) case the resolver falls back to cg.get_variable, so a normal
    # YAML-declared binary_sensor id still works.
    # One OR MORE wall switches per direction (like a light's on_buttons list).
    # cv.ensure_list accepts a single string ("has_board..._pin..") or a list of
    # them; a press on any up switch drives the group up, any down switch down.
    cv.Optional(CONF_WALL_UP): cv.ensure_list(cv.string_strict),
    cv.Optional(CONF_WALL_DOWN): cv.ensure_list(cv.string_strict),
    cv.Required(CONF_SHUTTERS): cv.ensure_list(cv.use_id(Shutter)),
    cv.Optional(CONF_CLICK_WIN, default="400ms"): cv.positive_time_period_milliseconds,
    cv.Optional(CONF_DEBOUNCE, default="80ms"): cv.positive_time_period_milliseconds,
    # Sun-based automation now lives on the group (moved from the shutter).
    # All actions default off, so an unconfigured group does nothing.
    cv.Optional(CONF_OPEN_AT_SUNRISE, default=False): cv.boolean,
    cv.Optional(CONF_CLOSE_AT_SUNSET, default=False): cv.boolean,
    cv.Optional(CONF_CLOSE_ON_SUN, default=False): cv.boolean,
    cv.Optional(CONF_SUNRISE_OFFSET, default="0min"): cv.positive_time_period_minutes,
    cv.Optional(CONF_SUNSET_OFFSET,  default="0min"): cv.positive_time_period_minutes,
    cv.Optional(CONF_SUPPRESS_SUNRISE, default=True): cv.boolean,
    cv.Optional(CONF_WINDOW): WINDOW_SCHEMA,
    cv.Optional(CONF_SUN_THROUGH_SENSOR): cv.use_id(binary_sensor.BinarySensor),
# ShutterGroup is a Component (loop() does wall-switch click counting), so the
# group schema must carry the component keys that cg.register_component reads.
}).extend(cv.COMPONENT_SCHEMA)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(ShutterHub),
    # Optional hub-level HA sub-device. When set, EVERY entity this hub creates
    # (shutter covers, group covers, per-shutter status/position telemetry,
    # per-group "overridden" binary_sensor, open-all + clear-overrides buttons)
    # is assigned to this one HA sub-device for cleaner device-page grouping.
    cv.Optional(CONF_DEVICE_ID): cv.use_id(Device),
    cv.Required(CONF_SUN_AZ): cv.use_id(sensor.Sensor),
    cv.Required(CONF_SUN_EL): cv.use_id(sensor.Sensor),
    cv.Optional(CONF_HEATING): cv.use_id(binary_sensor.BinarySensor),
    cv.Optional(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
    cv.Required(CONF_SHUTTERS): cv.ensure_list(SHUTTER_SCHEMA),
    cv.Optional(CONF_GROUPS, default=[]): cv.ensure_list(GROUP_SCHEMA),
    # Feature 1: single global time-of-day for the scheduled backup open.
    cv.Optional(CONF_BACKUP_OPEN_TIME): _parse_hhmm,
    # Feature 2: optional Open-All button. Accepts `{}` or `{name: ...}`.
    cv.Optional(CONF_OPEN_ALL_BUTTON): button.button_schema(
        OpenAllButton, icon="mdi:window-shutter-open"
    ).extend({
        cv.Optional(CONF_NAME, default=DEFAULT_OPEN_ALL_BUTTON_NAME): cv.string,
    }),
    # Feature 2 (overrides): optional Clear-Overrides button. Accepts `{}` or
    # `{name: ...}`. Pressing it is a manual reset_daily_flags().
    cv.Optional(CONF_CLEAR_OVERRIDES_BUTTON): button.button_schema(
        ClearOverridesButton, icon="mdi:restore"
    ).extend({
        cv.Optional(CONF_NAME, default=DEFAULT_CLEAR_OVERRIDES_BUTTON_NAME): cv.string,
    }),
}).extend(cv.COMPONENT_SCHEMA)


# ---------------------------------------------------------------------------
# Nested-use schema (for mcp23017_shared_int, which embeds a `shutter_hub:`
# block and generates the relay switches itself).
#
# Identical to CONFIG_SCHEMA except up_relay/down_relay are plain STRINGS (the
# generated `sw...` switch names), not use_id references. The embedding
# component resolves those names against its own board-generated switch map in
# codegen, so there is no validation-phase cross-component ID lookup (which
# cannot see entities created in another component's to_code).
# ---------------------------------------------------------------------------
NESTED_SHUTTER_SCHEMA = SHUTTER_SCHEMA.extend({
    cv.Required(CONF_UP_RELAY): cv.string,
    cv.Required(CONF_DOWN_RELAY): cv.string,
})

NESTED_CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(ShutterHub),
    # Same optional hub-level HA sub-device as the standalone schema; required
    # here for parity so the nested mcp23017_shared_int path also accepts it.
    cv.Optional(CONF_DEVICE_ID): cv.use_id(Device),
    cv.Required(CONF_SUN_AZ): cv.use_id(sensor.Sensor),
    cv.Required(CONF_SUN_EL): cv.use_id(sensor.Sensor),
    cv.Optional(CONF_HEATING): cv.use_id(binary_sensor.BinarySensor),
    cv.Optional(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
    cv.Required(CONF_SHUTTERS): cv.ensure_list(NESTED_SHUTTER_SCHEMA),
    cv.Optional(CONF_GROUPS, default=[]): cv.ensure_list(GROUP_SCHEMA),
    # Feature 1: single global time-of-day for the scheduled backup open.
    cv.Optional(CONF_BACKUP_OPEN_TIME): _parse_hhmm,
    # Feature 2: optional Open-All button. Accepts `{}` or `{name: ...}`.
    cv.Optional(CONF_OPEN_ALL_BUTTON): button.button_schema(
        OpenAllButton, icon="mdi:window-shutter-open"
    ).extend({
        cv.Optional(CONF_NAME, default=DEFAULT_OPEN_ALL_BUTTON_NAME): cv.string,
    }),
    # Feature 2 (overrides): optional Clear-Overrides button. Accepts `{}` or
    # `{name: ...}`. Pressing it is a manual reset_daily_flags().
    cv.Optional(CONF_CLEAR_OVERRIDES_BUTTON): button.button_schema(
        ClearOverridesButton, icon="mdi:restore"
    ).extend({
        cv.Optional(CONF_NAME, default=DEFAULT_CLEAR_OVERRIDES_BUTTON_NAME): cv.string,
    }),
}).extend(cv.COMPONENT_SCHEMA)


def _declare_entity_id(obj_id, cls):
    """Build a unique declaration ID for an entity synthesized at codegen time.

    text_sensor/sensor schemas auto-generate an ID via cv.GenerateID() during
    config validation; because these telemetry entities are created after
    validation, we mint the ID explicitly (same approach as the sibling
    mcp23017_shared_int component) so it is deterministic and unique.
    """
    base = sanitize(snake_case(obj_id))
    candidate = base
    i = 1
    while CORE.has_id(candidate):
        i += 1
        candidate = f"{base}_{i}"
    the_id = ID(candidate, is_declaration=True, type=cls)
    return the_id


async def to_code(config):
    # Standalone use: relays are resolved as normal switch IDs via use_id.
    await build_shutter_hub(config)


async def build_shutter_hub(config, relay_resolver=None, button_resolver=None):
    """Generate the full ShutterHub graph (hub, shutters, groups, telemetry).

    Shared by the standalone `shutter_hub:` component and by
    `mcp23017_shared_int`, which nests this config and generates the relay
    switches itself. ``relay_resolver`` maps an up_relay/down_relay reference to
    the switch var to wire. When None (standalone), relays are plain switch IDs
    resolved via cg.get_variable (the original behavior). When provided (nested
    in mcp23017_shared_int), the reference is a `sw...` name string resolved
    against the board-generated switch map, so no validation-phase use_id is
    needed.
    """
    if relay_resolver is None:
        async def relay_resolver(ref):
            return await cg.get_variable(ref)

    # button_resolver maps a wall_up/wall_down reference to the binary_sensor var.
    # Standalone: resolve a normal YAML-declared binary_sensor id via get_variable.
    # Nested in mcp23017_shared_int: a "has_board..._pin.." string resolved against
    # the board-generated input sensor map (fabricated at codegen, so not
    # resolvable by validation-phase use_id).
    if button_resolver is None:
        async def button_resolver(ref):
            # Standalone: wall_up/wall_down are bare id strings (GROUP_SCHEMA
            # validates them as cv.string_strict so the nested
            # mcp23017_shared_int path can use codegen-fabricated ids). Here the
            # referenced binary_sensor is a real, validation-time entity, so wrap
            # the string into an ID of the right type before cg.get_variable
            # (which requires an ID, not a str).
            return await cg.get_variable(
                ID(ref, is_declaration=False, type=binary_sensor.BinarySensor))

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Optional hub-level HA sub-device. ID object (from cv.use_id) or None. When
    # present it is injected into each entity's config dict below BEFORE the
    # stock new_* helper runs; those helpers' @setup_entity core setup reads
    # CONF_DEVICE_ID and emits set_device_, grouping every entity under this one
    # HA sub-device. Strictly a no-op when omitted.
    dev_id = config.get(CONF_DEVICE_ID)

    cg.add(var.set_sun_azimuth(await cg.get_variable(config[CONF_SUN_AZ])))
    cg.add(var.set_sun_elevation(await cg.get_variable(config[CONF_SUN_EL])))
    if CONF_HEATING in config:
        cg.add(var.set_heating_needed(await cg.get_variable(config[CONF_HEATING])))
    if CONF_TIME_ID in config:
        cg.add(var.set_time(await cg.get_variable(config[CONF_TIME_ID])))

    for i, s in enumerate(config[CONF_SHUTTERS]):
        if dev_id is not None:
            s[CONF_DEVICE_ID] = dev_id
        sv = await cover.new_cover(s)
        cg.add(sv.set_hub(var))
        cg.add(sv.set_index(i))
        cg.add(sv.set_up_relay(await relay_resolver(s[CONF_UP_RELAY])))
        cg.add(sv.set_down_relay(await relay_resolver(s[CONF_DOWN_RELAY])))
        cg.add(sv.set_travel_time(s[CONF_TRAVEL_TIME]))
        cg.add(sv.set_restore(s[CONF_RESTORE]))

        base_name = s.get(CONF_NAME) or str(s[CONF_ID])
        if s[CONF_STATUS_SENSOR]:
            status_conf = text_sensor.text_sensor_schema(
                icon="mdi:window-shutter-cog",
            )({
                CONF_ID: _declare_entity_id(
                    f"{base_name}_status", text_sensor.TextSensor),
                CONF_NAME: f"{base_name} Status",
            })
            if dev_id is not None:
                status_conf[CONF_DEVICE_ID] = dev_id
            status = await text_sensor.new_text_sensor(status_conf)
            cg.add(sv.set_status_sensor(status))
        if s[CONF_POSITION_SENSOR]:
            pos_conf = sensor.sensor_schema(
                unit_of_measurement="%",
                accuracy_decimals=0,
                icon="mdi:window-shutter",
            )({
                CONF_ID: _declare_entity_id(
                    f"{base_name}_position", sensor.Sensor),
                CONF_NAME: f"{base_name} Position",
            })
            if dev_id is not None:
                pos_conf[CONF_DEVICE_ID] = dev_id
            pos = await sensor.new_sensor(pos_conf)
            cg.add(sv.set_position_sensor(pos))

        cg.add(var.add_shutter(sv))

    for g in config[CONF_GROUPS]:
        if dev_id is not None:
            g[CONF_DEVICE_ID] = dev_id
        gv = await cover.new_cover(g)
        # ShutterGroup is a Component (its loop() does all wall-switch click
        # counting) as well as a Cover entity. cover.new_cover() registers only
        # the entity, NOT the component, so without this call ESPHome never ran
        # the group's setup()/loop() and wall-switch clicks were silently never
        # counted (HA commands still worked because those go through control()).
        await cg.register_component(gv, g)
        cg.add(gv.set_hub(var))
        # Bind each wall switch's press EDGE to the group. The sensor is kept on
        # the group (add_wall_up/add_wall_down) AND its state callback notifies
        # the group the instant it sees the OFF->ON edge, so a bouncy press's
        # brief ON is never lost between main-loop level samples.
        #
        # We use add_on_state_callback (a binary_sensor::BinarySensor base-class
        # method, so it works for stock gpio sensors AND the fabricated
        # mcp23017_shared_int sensors alike) with an `if (s)` guard so the group
        # is notified only on the ON (press) edge, not on release — matching the
        # press-edge-only semantics and avoiding double counting.
        gname = str(gv)  # file-scope C++ var name referenced inside the lambdas
        for ref in g.get(CONF_WALL_UP, []):
            sv = await button_resolver(ref)
            cg.add(gv.add_wall_up(sv))
            cg.add(sv.add_on_state_callback(
                cg.RawExpression(f"[](bool s) {{ if (s) {gname}->on_wall_up_edge(); }}")))
        for ref in g.get(CONF_WALL_DOWN, []):
            sv = await button_resolver(ref)
            cg.add(gv.add_wall_down(sv))
            cg.add(sv.add_on_state_callback(
                cg.RawExpression(f"[](bool s) {{ if (s) {gname}->on_wall_down_edge(); }}")))
        cg.add(gv.set_click_window(g[CONF_CLICK_WIN]))
        cg.add(gv.set_debounce(g[CONF_DEBOUNCE]))
        cg.add(gv.set_open_at_sunrise(g[CONF_OPEN_AT_SUNRISE]))
        cg.add(gv.set_close_at_sunset(g[CONF_CLOSE_AT_SUNSET]))
        cg.add(gv.set_sunrise_offset(g[CONF_SUNRISE_OFFSET]))
        cg.add(gv.set_sunset_offset(g[CONF_SUNSET_OFFSET]))
        cg.add(gv.set_suppress_sunrise_if_sun_through(g[CONF_SUPPRESS_SUNRISE]))
        cg.add(gv.set_close_on_sun_through(g[CONF_CLOSE_ON_SUN]))
        if CONF_WINDOW in g:
            w = g[CONF_WINDOW]
            cg.add(gv.set_window(w[CONF_W], w[CONF_D], w[CONF_H],
                                 w[CONF_ORIENT], w[CONF_MARGIN]))
        if CONF_SUN_THROUGH_SENSOR in g:
            cg.add(gv.set_sun_through_sensor(
                await cg.get_variable(g[CONF_SUN_THROUGH_SENSOR])))
        # Per-group "overridden today" binary_sensor, fabricated at codegen the
        # same way the per-shutter telemetry entities are (always on, no config).
        # A plain binary_sensor::BinarySensor (EntityBase only), registered via
        # App.register_binary_sensor, NOT a counted Component.
        group_name = g.get(CONF_NAME) or str(g[CONF_ID])
        ov_conf = binary_sensor.binary_sensor_schema(
            icon="mdi:gesture-tap",
        )({
            CONF_ID: _declare_entity_id(
                f"{g[CONF_ID]}_overridden", binary_sensor.BinarySensor),
            CONF_NAME: f"{group_name} Overridden",
        })
        if dev_id is not None:
            ov_conf[CONF_DEVICE_ID] = dev_id
        ov = await binary_sensor.new_binary_sensor(ov_conf)
        cg.add(gv.set_overridden_sensor(ov))
        for sh in g[CONF_SHUTTERS]:
            cg.add(gv.add_shutter(await cg.get_variable(sh)))
        cg.add(var.add_group(gv))

    # Feature 1: scheduled backup open. The value is already parsed to
    # (hour, minute) by _parse_hhmm during validation. Requires a time source.
    if CONF_BACKUP_OPEN_TIME in config:
        if CONF_TIME_ID not in config:
            raise cv.Invalid(
                f"'{CONF_BACKUP_OPEN_TIME}' requires a '{CONF_TIME_ID}:' "
                "(RealTimeClock) to be configured."
            )
        hour, minute = config[CONF_BACKUP_OPEN_TIME]
        cg.add(var.set_backup_open_time(hour, minute))

    # Feature 2: Open-All button. The button is a button::Button (EntityBase
    # only), registered via App.register_button, so it is NOT a counted
    # component and needs no fabricated-id entry.
    if CONF_OPEN_ALL_BUTTON in config:
        if dev_id is not None:
            config[CONF_OPEN_ALL_BUTTON][CONF_DEVICE_ID] = dev_id
        btn = await button.new_button(config[CONF_OPEN_ALL_BUTTON])
        cg.add(btn.set_hub(var))

    # Feature 2 (overrides): Clear-Overrides button. Like the Open-All button it
    # is a button::Button (EntityBase only), registered via App.register_button,
    # so it is NOT a counted component and needs no fabricated-id entry. Pressing
    # it runs a manual reset_daily_flags().
    if CONF_CLEAR_OVERRIDES_BUTTON in config:
        if dev_id is not None:
            config[CONF_CLEAR_OVERRIDES_BUTTON][CONF_DEVICE_ID] = dev_id
        btn = await button.new_button(config[CONF_CLEAR_OVERRIDES_BUTTON])
        cg.add(btn.set_hub(var))

    # Expose the hub var so an embedding component (mcp23017_shared_int) can wire
    # it into its sun_schedule handler. The standalone to_code ignores the return.
    return var
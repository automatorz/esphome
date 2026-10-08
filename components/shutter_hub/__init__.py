import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import (
    cover, binary_sensor, sensor, sun, switch as switch_, time as time_,
)
from esphome.const import CONF_ID, CONF_NAME, CONF_DEVICE_CLASS

CODEOWNERS = ["@you"]
DEPENDENCIES = ["sun"]
AUTO_LOAD = ["cover", "binary_sensor"]

ns = cg.esphome_ns.namespace("shutter_hub")
ShutterHub   = ns.class_("ShutterHub", cg.Component)
Shutter      = ns.class_("Shutter", cover.Cover, cg.Component)
ShutterGroup = ns.class_("ShutterGroup", cover.Cover, cg.Component)

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
CONF_RESTORE             = "restore"
CONF_TIME_ID             = "time_id"

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
    cv.Optional(CONF_OPEN_AT_SUNRISE, default=True): cv.boolean,
    cv.Optional(CONF_CLOSE_AT_SUNSET, default=True): cv.boolean,
    cv.Optional(CONF_SUNRISE_OFFSET, default="0min"): cv.positive_time_period_minutes,
    cv.Optional(CONF_SUNSET_OFFSET,  default="0min"): cv.positive_time_period_minutes,
    cv.Optional(CONF_SUPPRESS_SUNRISE, default=True): cv.boolean,
    cv.Optional(CONF_WINDOW): WINDOW_SCHEMA,
    cv.Optional(CONF_CLOSE_ON_SUN, default=True): cv.boolean,
    cv.Optional(CONF_RESTORE, default=True): cv.boolean,
    cv.Optional(CONF_SUN_THROUGH_SENSOR): cv.use_id(binary_sensor.BinarySensor),
})

# wall_up / wall_down are now optional. If both are omitted, the group
# still exists as a cover entity and can be commanded via HA, but it has
# no physical wall-switch control.
GROUP_SCHEMA = cover.cover_schema(ShutterGroup).extend({
    cv.Optional(CONF_DEVICE_CLASS, default="shutter"): cv.string,
    cv.Optional(CONF_WALL_UP): cv.use_id(binary_sensor.BinarySensor),
    cv.Optional(CONF_WALL_DOWN): cv.use_id(binary_sensor.BinarySensor),
    cv.Required(CONF_SHUTTERS): cv.ensure_list(cv.use_id(Shutter)),
    cv.Optional(CONF_CLICK_WIN, default="400ms"): cv.positive_time_period_milliseconds,
})

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(ShutterHub),
    cv.Required(CONF_SUN_AZ): cv.use_id(sensor.Sensor),
    cv.Required(CONF_SUN_EL): cv.use_id(sensor.Sensor),
    cv.Optional(CONF_HEATING): cv.use_id(binary_sensor.BinarySensor),
    cv.Optional(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
    cv.Required(CONF_SHUTTERS): cv.ensure_list(SHUTTER_SCHEMA),
    cv.Optional(CONF_GROUPS, default=[]): cv.ensure_list(GROUP_SCHEMA),
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_sun_azimuth(await cg.get_variable(config[CONF_SUN_AZ])))
    cg.add(var.set_sun_elevation(await cg.get_variable(config[CONF_SUN_EL])))
    if CONF_HEATING in config:
        cg.add(var.set_heating_needed(await cg.get_variable(config[CONF_HEATING])))
    if CONF_TIME_ID in config:
        cg.add(var.set_time(await cg.get_variable(config[CONF_TIME_ID])))

    for i, s in enumerate(config[CONF_SHUTTERS]):
        sv = await cover.new_cover(s)
        cg.add(sv.set_hub(var))
        cg.add(sv.set_index(i))
        cg.add(sv.set_up_relay(await cg.get_variable(s[CONF_UP_RELAY])))
        cg.add(sv.set_down_relay(await cg.get_variable(s[CONF_DOWN_RELAY])))
        cg.add(sv.set_travel_time(s[CONF_TRAVEL_TIME]))
        cg.add(sv.set_open_at_sunrise(s[CONF_OPEN_AT_SUNRISE]))
        cg.add(sv.set_close_at_sunset(s[CONF_CLOSE_AT_SUNSET]))
        cg.add(sv.set_sunrise_offset(s[CONF_SUNRISE_OFFSET]))
        cg.add(sv.set_sunset_offset(s[CONF_SUNSET_OFFSET]))
        cg.add(sv.set_suppress_sunrise_if_sun_through(s[CONF_SUPPRESS_SUNRISE]))
        cg.add(sv.set_close_on_sun_through(s[CONF_CLOSE_ON_SUN]))
        cg.add(sv.set_restore(s[CONF_RESTORE]))
        if CONF_WINDOW in s:
            w = s[CONF_WINDOW]
            cg.add(sv.set_window(w[CONF_W], w[CONF_D], w[CONF_H],
                                 w[CONF_ORIENT], w[CONF_MARGIN]))
        if CONF_SUN_THROUGH_SENSOR in s:
            cg.add(sv.set_sun_through_sensor(
                await cg.get_variable(s[CONF_SUN_THROUGH_SENSOR])))
        cg.add(var.add_shutter(sv))

    for g in config[CONF_GROUPS]:
        gv = await cover.new_cover(g)
        cg.add(gv.set_hub(var))
        if CONF_WALL_UP in g:
            cg.add(gv.set_wall_up(await cg.get_variable(g[CONF_WALL_UP])))
        if CONF_WALL_DOWN in g:
            cg.add(gv.set_wall_down(await cg.get_variable(g[CONF_WALL_DOWN])))
        cg.add(gv.set_click_window(g[CONF_CLICK_WIN]))
        for sh in g[CONF_SHUTTERS]:
            cg.add(gv.add_shutter(await cg.get_variable(sh)))
        cg.add(var.add_group(gv))
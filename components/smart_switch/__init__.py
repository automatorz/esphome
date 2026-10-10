import re

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch, time
from esphome.const import CONF_ID

smart_switch_ns = cg.esphome_ns.namespace("smart_switch")
SmartSwitch = smart_switch_ns.class_("SmartSwitch", cg.Component)

CONF_TARGET_SWITCH       = "target_switch"
CONF_DEBUG               = "debug"
CONF_INITIAL_STATE       = "initial_state"
CONF_FORCE_INITIAL_STATE = "force_initial_state"
CONF_UPDATE_INTERVAL     = "update_interval"
CONF_SCHEDULE            = "schedule"
CONF_START_OFFSET        = "start_offset"
CONF_PERIOD              = "period"
CONF_DURATION            = "duration"
CONF_INVERT              = "invert"
CONF_TIME_ID             = "time_id"


# ---------------------------------------------------------------------------
# Time-period validator supporting s / m / h / d suffixes (plain int = seconds)
# ---------------------------------------------------------------------------
_UNIT_TO_SECONDS = {"s": 1, "m": 60, "h": 3600, "d": 86400}
_TIME_PERIOD_RE = re.compile(r"^\s*(\d+)\s*([smhd])\s*$")


def time_period_seconds(value):
    if isinstance(value, bool):
        raise cv.Invalid("Boolean is not a valid time period")
    if isinstance(value, int):
        return value
    s = cv.string(value)
    m = _TIME_PERIOD_RE.match(s)
    if m is None:
        raise cv.Invalid(
            f"Invalid time period '{s}'. Expected an integer (seconds) or a "
            f"value with a unit suffix 's', 'm', 'h', or 'd' "
            f"(e.g. '30s', '15m', '2h', '1d')."
        )
    amount, unit = int(m.group(1)), m.group(2)
    return amount * _UNIT_TO_SECONDS[unit]


SCHEDULE_SCHEMA = cv.Schema({
    cv.Optional(CONF_START_OFFSET, default=0): time_period_seconds,
    cv.Required(CONF_PERIOD):                  time_period_seconds,
    cv.Required(CONF_DURATION):                time_period_seconds,
    cv.Optional(CONF_INVERT, default=False):   cv.boolean,
    cv.Required(CONF_TIME_ID):                 cv.use_id(time.RealTimeClock),
})

# Per-instance schema
SMART_SWITCH_SCHEMA = cv.Schema({
    cv.Required(CONF_ID):                            cv.declare_id(SmartSwitch),
    cv.Required(CONF_TARGET_SWITCH):                 cv.use_id(switch.Switch),
    cv.Optional(CONF_DEBUG, default=False):          cv.boolean,
    cv.Optional(CONF_INITIAL_STATE, default=False):  cv.boolean,
    cv.Optional(CONF_FORCE_INITIAL_STATE, default=False): cv.boolean,
    cv.Optional(CONF_UPDATE_INTERVAL, default="10s"):
        cv.positive_time_period_milliseconds,
    cv.Optional(CONF_SCHEDULE):                      SCHEDULE_SCHEMA,
}).extend(cv.COMPONENT_SCHEMA)

# Top-level schema: accepts a single dict OR a list of dicts
CONFIG_SCHEMA = cv.ensure_list(SMART_SWITCH_SCHEMA)


async def to_code(configs):
    # If the user wrote a single dict, cv.ensure_list already wrapped it.
    for config in configs:
        var = cg.new_Pvariable(config[CONF_ID])
        await cg.register_component(var, config)

        target = await cg.get_variable(config[CONF_TARGET_SWITCH])
        cg.add(var.set_target_switch(target))

        cg.add(var.set_debug(config[CONF_DEBUG]))
        cg.add(var.set_initial_state(config[CONF_INITIAL_STATE]))
        cg.add(var.set_force_initial_state(config[CONF_FORCE_INITIAL_STATE]))
        cg.add(var.set_update_interval(config[CONF_UPDATE_INTERVAL]))

        if CONF_SCHEDULE in config:
            sched = config[CONF_SCHEDULE]
            timer_var = await cg.get_variable(sched[CONF_TIME_ID])
            cg.add(var.set_schedule_enabled(True))
            cg.add(var.set_schedule_start_offset(sched[CONF_START_OFFSET]))
            cg.add(var.set_schedule_period(sched[CONF_PERIOD]))
            cg.add(var.set_schedule_duration(sched[CONF_DURATION]))
            cg.add(var.set_schedule_invert(sched[CONF_INVERT]))
            cg.add(var.set_schedule_time(timer_var))

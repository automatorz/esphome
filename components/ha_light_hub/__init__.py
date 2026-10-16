"""ha_light_hub external component — HA light dimmer driven by MCP23017 buttons.

A small sibling of `shutter_hub`, embedded as a nested key `ha_light_hub:` under
`mcp23017_shared_int:`. Each `dimmers:` entry generates one `HaLightDimmer` C++
Component that controls one EXTERNAL Home Assistant light entity via
light.turn_on / light.turn_off HA API actions. Physical MCP23017 input buttons
notify the dimmer on their OFF->ON press edge through the binary sensor's generic
on_press_ hook.

This module is NOT auto-loaded and NOT a validated top-level component: its
sources are copied via the parent's codegen-time source-copy sentinel
(CORE.config["ha_light_hub"] = [] + CORE.loaded_integrations.add(...)), exactly
like shutter_hub. Its DEPENDENCIES list below is therefore documentation ONLY —
it is inert on the sentinel load path (MetadataValidationStep never runs for this
domain) and MUST NOT be relied on. The parent guards `api:` explicitly; the
sensor / text_sensor references are guarded by cg.get_variable at codegen.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor, text_sensor
from esphome.const import CONF_ID

CODEOWNERS = ["@automatorz"]
# Documentation only — inert on the sentinel load path (see module docstring).
DEPENDENCIES = ["api", "sensor", "text_sensor"]

ns = cg.esphome_ns.namespace("ha_light_hub")
HaLightDimmer = ns.class_("HaLightDimmer", cg.Component)

# --- config keys ---
CONF_DIMMERS = "dimmers"
CONF_ENTITY_ID = "entity_id"
CONF_BRIGHTNESS_SENSOR = "brightness_sensor"
CONF_STATE_SENSOR = "state_sensor"
CONF_LEVELS_PCT = "levels_pct"
CONF_TRANSITION = "transition"
CONF_DOUBLE_CLICK_WINDOW = "double_click_window"
CONF_DEBOUNCE = "debounce"
CONF_UP_BUTTONS = "up_buttons"
CONF_DOWN_BUTTONS = "down_buttons"


def _ascending(value):
    """Reject a non-strictly-ascending levels list."""
    for a, b in zip(value, value[1:]):
        if b <= a:
            raise cv.Invalid(f"levels_pct must be strictly ascending; got {value}")
    return value


DIMMER_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(HaLightDimmer),
        cv.Required(CONF_ENTITY_ID): cv.All(cv.string, cv.Length(min=1)),
        cv.Required(CONF_BRIGHTNESS_SENSOR): cv.use_id(sensor.Sensor),
        cv.Required(CONF_STATE_SENSOR): cv.use_id(text_sensor.TextSensor),
        cv.Required(CONF_LEVELS_PCT): cv.All(
            cv.ensure_list(cv.int_range(min=1, max=100)),
            cv.Length(min=2),
            _ascending,
        ),
        cv.Optional(CONF_TRANSITION, default="1s"): cv.positive_time_period_milliseconds,
        cv.Optional(
            CONF_DOUBLE_CLICK_WINDOW, default="350ms"
        ): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_DEBOUNCE, default="20ms"): cv.positive_time_period_milliseconds,
        cv.Required(CONF_UP_BUTTONS): cv.All(cv.ensure_list(cv.string), cv.Length(min=1)),
        cv.Required(CONF_DOWN_BUTTONS): cv.All(cv.ensure_list(cv.string), cv.Length(min=1)),
    }
).extend(cv.COMPONENT_SCHEMA)

NESTED_CONFIG_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_DIMMERS): cv.ensure_list(DIMMER_SCHEMA),
    }
)


async def build_ha_light_hub(config, button_resolver):
    """Generate one HaLightDimmer per `dimmers:` entry.

    ``button_resolver`` is an async callable mapping an up/down button id string
    (e.g. "has_board04_chip01_pin00") to the MCP23017BinarySensor var it names.
    """
    for conf in config[CONF_DIMMERS]:
        var = cg.new_Pvariable(conf[CONF_ID])
        await cg.register_component(var, conf)

        cg.add(var.set_entity_id(conf[CONF_ENTITY_ID]))  # Python str -> C++ const char*
        cg.add(var.set_brightness_sensor(await cg.get_variable(conf[CONF_BRIGHTNESS_SENSOR])))
        cg.add(var.set_state_sensor(await cg.get_variable(conf[CONF_STATE_SENSOR])))

        csv = ", ".join(str(x) for x in conf[CONF_LEVELS_PCT])
        cg.add(var.set_levels(cg.RawExpression(f"std::vector<uint8_t>{{{csv}}}")))

        cg.add(var.set_transition(conf[CONF_TRANSITION].total_milliseconds / 1000.0))
        cg.add(var.set_double_click_window(conf[CONF_DOUBLE_CLICK_WINDOW].total_milliseconds))
        cg.add(var.set_debounce(conf[CONF_DEBOUNCE].total_milliseconds))

        # Overlap guard: a button must drive only one direction (per dimmer).
        overlap = set(conf[CONF_UP_BUTTONS]) & set(conf[CONF_DOWN_BUTTONS])
        if overlap:
            raise cv.Invalid(
                f"ha_light_hub dimmer button(s) {sorted(overlap)} appear in both "
                f"up_buttons and down_buttons; a button must drive only one direction."
            )

        name = str(var)  # file-scope global identifier referenced inside the lambda
        for ref in conf[CONF_UP_BUTTONS]:
            sv = await button_resolver(ref)
            cg.add(sv.add_on_press(cg.RawExpression(f"[]() {{ {name}->on_up_pressed(); }}")))
        for ref in conf[CONF_DOWN_BUTTONS]:
            sv = await button_resolver(ref)
            cg.add(sv.add_on_press(cg.RawExpression(f"[]() {{ {name}->on_down_pressed(); }}")))

    # Mandatory: the user's api: leaves homeassistant_services: false, so the HA
    # services header would otherwise compile to nothing. Force the define once.
    cg.add_define("USE_API_HOMEASSISTANT_SERVICES")

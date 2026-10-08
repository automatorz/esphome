"""Switch platform for MCP23017 shared-interrupt output pins.

Each switch references a chip + pin and drives the pin via OLAT. Outputs are
independent of the interrupt path.
"""

import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv
from esphome.const import CONF_PIN

from . import MCP23017Chip, mcp23017_shared_int_ns

DEPENDENCIES = ["mcp23017_shared_int"]

CONF_CHIP = "chip"

MCP23017Switch = mcp23017_shared_int_ns.class_(
    "MCP23017Switch", switch.Switch, cg.Component
)

CONFIG_SCHEMA = switch.switch_schema(MCP23017Switch).extend(
    {
        cv.Required(CONF_CHIP): cv.use_id(MCP23017Chip),
        cv.Required(CONF_PIN): cv.int_range(min=0, max=15),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = await switch.new_switch(config)
    await cg.register_component(var, config)

    chip = await cg.get_variable(config[CONF_CHIP])
    cg.add(var.set_chip(chip))
    cg.add(var.set_pin(config[CONF_PIN]))

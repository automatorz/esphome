"""Passive binary_sensor platform for MCP23017 shared-interrupt input pins.

Each sensor references the coordinator (which owns the shared ISR) and a specific
chip + pin. The sensor itself never reads hardware: the coordinator pushes state to
it when the shared interrupt fires and this pin changed.
"""

import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import CONF_INVERTED, CONF_PIN

from . import (
    MCP23017Chip,
    SharedIntCoordinator,
    mcp23017_shared_int_ns,
)

DEPENDENCIES = ["mcp23017_shared_int"]

CONF_MCP23017_SHARED_INT = "mcp23017_shared_int"
CONF_CHIP = "chip"

MCP23017BinarySensor = mcp23017_shared_int_ns.class_(
    "MCP23017BinarySensor", binary_sensor.BinarySensor, cg.Component
)

CONFIG_SCHEMA = binary_sensor.binary_sensor_schema(MCP23017BinarySensor).extend(
    {
        cv.GenerateID(CONF_MCP23017_SHARED_INT): cv.use_id(SharedIntCoordinator),
        cv.Required(CONF_CHIP): cv.use_id(MCP23017Chip),
        cv.Required(CONF_PIN): cv.int_range(min=0, max=15),
        cv.Optional(CONF_INVERTED, default=False): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
    await cg.register_component(var, config)

    coord = await cg.get_variable(config[CONF_MCP23017_SHARED_INT])
    chip = await cg.get_variable(config[CONF_CHIP])

    cg.add(var.set_coordinator(coord))
    cg.add(var.set_chip(chip))
    cg.add(var.set_pin(config[CONF_PIN]))
    cg.add(var.set_inverted(config[CONF_INVERTED]))

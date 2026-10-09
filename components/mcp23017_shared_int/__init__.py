"""MCP23017 shared-interrupt external component.

Fully interrupt-driven support for multiple MCP23017 expanders whose INT lines are
wired-OR onto a single ESP32 GPIO. One ISR on the shared pin drives all reads; no
polling anywhere on the data path.

Two ways to declare chips:

1. Low-level ``chips:`` list — register raw MCP23017 hubs, then claim pins via the
   ``binary_sensor`` / ``switch`` platforms.

2. High-level ``boards:`` list — a "board" is 2 MCP23017 chips on one I2C bus with
   per-bank direction. Each board auto-generates all 32 pin entities (16 inputs or
   outputs per bank-pair) with configurable names, ids, inversion and physical
   location labels. This replaces the nested !include package files.

Board config shape::

    mcp23017_shared_int:
      id: io
      interrupt_pin: GPIO14
      boards:
        - board_id: "01"
          i2c_id: bus_a
          chip1_address: 0x20
          chip2_address: 0x21
          bank_a: { direction: input }      # bottom row
          bank_b: { direction: output }     # top row

A board expands into:
  * chips ``mcp23017_b<board_id>_c01`` and ``..._c02``
  * for each input pin:  a binary_sensor id ``has_board<bb>_chip<cc>_pin<pp>``
                         name ``<prefix> Board<bb> <location> (Chip<cc> Pin<pp>)``
  * for each output pin: an output-backed switch id ``sw<bb>_chip<cc>_pin<pp>``
                         name ``<prefix> Board<bb> <location> (Chip<cc> Pin<pp>)``
"""

from esphome import pins
import esphome.codegen as cg
from esphome.components import binary_sensor, i2c, switch
import esphome.config_validation as cv
from esphome.core import CORE, ID
from esphome.const import (
    CONF_ADDRESS,
    CONF_DIRECTION,
    CONF_ID,
    CONF_INTERRUPT_PIN,
    CONF_INVERTED,
    CONF_NAME,
)

CODEOWNERS = ["@automatorz"]
DEPENDENCIES = ["i2c"]
AUTO_LOAD = ["binary_sensor", "switch"]
MULTI_CONF = False

mcp23017_shared_int_ns = cg.esphome_ns.namespace("mcp23017_shared_int")

SharedIntCoordinator = mcp23017_shared_int_ns.class_(
    "SharedIntCoordinator", cg.Component
)
MCP23017Chip = mcp23017_shared_int_ns.class_(
    "MCP23017Chip", cg.Component, i2c.I2CDevice
)
MCP23017BinarySensor = mcp23017_shared_int_ns.class_(
    "MCP23017BinarySensor", binary_sensor.BinarySensor, cg.Component
)
MCP23017Switch = mcp23017_shared_int_ns.class_(
    "MCP23017Switch", switch.Switch, cg.Component
)

# --- config keys ---
CONF_CHIPS = "chips"
CONF_BOARDS = "boards"
CONF_BOARD_ID = "board_id"
CONF_I2C_ID = "i2c_id"
CONF_CHIP1_ADDRESS = "chip1_address"
CONF_CHIP2_ADDRESS = "chip2_address"
CONF_BANK_A = "bank_a"
CONF_BANK_B = "bank_b"
CONF_NAME_PREFIX = "name_prefix"
CONF_LOCATION_PREFIX = "location_prefix"
CONF_LOCATIONS = "locations"

DIR_INPUT = "input"
DIR_OUTPUT = "output"

# Default Home Assistant name prefixes for auto-generated bank entities.
# The "ZZ" prefix on outputs makes them sort last alphabetically in Home
# Assistant, keeping inputs grouped ahead of outputs in entity lists.
DEFAULT_INPUT_NAME_PREFIX = "HAS"
DEFAULT_OUTPUT_NAME_PREFIX = "ZZHAS"

# ---------------------------------------------------------------------------
# Low-level chips schema (unchanged, still supported)
# ---------------------------------------------------------------------------
CHIP_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.declare_id(MCP23017Chip),
    }
).extend(i2c.i2c_device_schema(0x20))


# ---------------------------------------------------------------------------
# High-level board / bank schema
# ---------------------------------------------------------------------------
def _bank_schema(default_location_prefix, default_name_prefix):
    return cv.Schema(
        {
            cv.Required(CONF_DIRECTION): cv.one_of(
                DIR_INPUT, DIR_OUTPUT, lower=True
            ),
            cv.Optional(CONF_INVERTED, default=True): cv.boolean,
            cv.Optional(CONF_NAME_PREFIX): cv.string,
            cv.Optional(
                CONF_LOCATION_PREFIX, default=default_location_prefix
            ): cv.string,
            # Optional explicit 16-entry label list (across both chips, physical
            # position 1..16). Overrides the auto-generated labels.
            cv.Optional(CONF_LOCATIONS): cv.All(
                cv.ensure_list(cv.string), cv.Length(min=16, max=16)
            ),
        }
    )


BOARD_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_BOARD_ID): cv.string,
        cv.Required(CONF_I2C_ID): cv.use_id(i2c.I2CBus),
        cv.Optional(CONF_CHIP1_ADDRESS, default=0x20): cv.i2c_address,
        cv.Optional(CONF_CHIP2_ADDRESS, default=0x21): cv.i2c_address,
        cv.Required(CONF_BANK_A): _bank_schema("Bottom", "HAS"),
        cv.Required(CONF_BANK_B): _bank_schema("Top", "HAS"),
    }
)


def _validate_at_least_one(config):
    if CONF_CHIPS not in config and CONF_BOARDS not in config:
        raise cv.Invalid(
            f"You must define at least one of '{CONF_CHIPS}' or '{CONF_BOARDS}'."
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SharedIntCoordinator),
            cv.Required(CONF_INTERRUPT_PIN): pins.internal_gpio_input_pin_schema,
            cv.Optional(CONF_CHIPS): cv.All(
                cv.ensure_list(CHIP_SCHEMA), cv.Length(min=1, max=16)
            ),
            cv.Optional(CONF_BOARDS): cv.All(
                cv.ensure_list(BOARD_SCHEMA), cv.Length(min=1, max=16)
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_at_least_one,
)


# ---------------------------------------------------------------------------
# Label generation
# ---------------------------------------------------------------------------
def _default_location(location_prefix, chip_index, pin, is_bank_a):
    """Physical label for a pin.

    Bank A (bottom): position = chip_index*8 + pin + 1    (ascending)
    Bank B (top):    position = chip_index*8 + (16 - pin)  (descending within chip)
    """
    if is_bank_a:
        pos = chip_index * 8 + pin + 1
    else:
        pos = chip_index * 8 + (16 - pin)
    return f"{location_prefix} {pos:02d}"


def _location_for(bank_conf, chip_index, pin, is_bank_a):
    locations = bank_conf.get(CONF_LOCATIONS)
    if locations is not None:
        # Explicit list indexed by physical position (1..16) -> 0-based.
        if is_bank_a:
            pos = chip_index * 8 + pin + 1
        else:
            pos = chip_index * 8 + (16 - pin)
        return locations[pos - 1]
    return _default_location(
        bank_conf[CONF_LOCATION_PREFIX], chip_index, pin, is_bank_a
    )


# ---------------------------------------------------------------------------
# Entity generation
# ---------------------------------------------------------------------------
def _declare_component_id(obj_id, cls):
    """Create a declaration ID for a Component synthesized at codegen time and
    register it in CORE.component_ids so cg.register_component() accepts it.
    (Normally cv.declare_id does this during config validation; our board pins
    are generated after validation, so we register them here.)"""
    the_id = ID(obj_id, is_declaration=True, type=cls)
    CORE.component_ids.add(the_id.id)
    return the_id


async def _gen_input_pin(coord, chip, board_id, chip_id, pin, name_prefix, location, inverted):
    pin2 = f"{pin:02d}"
    name = f"{name_prefix} Board{board_id} {location} (Chip{chip_id} Pin{pin2})"
    obj_id = f"has_board{board_id}_chip{chip_id}_pin{pin2}"

    conf = {
        CONF_ID: _declare_component_id(obj_id, MCP23017BinarySensor),
        CONF_NAME: name,
    }
    conf = binary_sensor.binary_sensor_schema(MCP23017BinarySensor)(conf)

    var = await binary_sensor.new_binary_sensor(conf)
    await cg.register_component(var, conf)
    cg.add(var.set_coordinator(coord))
    cg.add(var.set_chip(chip))
    cg.add(var.set_pin(pin))
    cg.add(var.set_inverted(inverted))


async def _gen_output_pin(chip, board_id, chip_id, pin, name_prefix, location, inverted):
    pin2 = f"{pin:02d}"
    name = f"{name_prefix} Board{board_id} {location} (Chip{chip_id} Pin{pin2})"
    obj_id = f"sw{board_id}_chip{chip_id}_pin{pin2}"

    conf = {
        CONF_ID: _declare_component_id(obj_id, MCP23017Switch),
        CONF_NAME: name,
        CONF_INVERTED: inverted,
    }
    conf = switch.switch_schema(MCP23017Switch, default_restore_mode="ALWAYS_OFF")(conf)

    var = await switch.new_switch(conf)
    await cg.register_component(var, conf)
    cg.add(var.set_chip(chip))
    cg.add(var.set_pin(pin))


async def _gen_bank(coord, chip, board_id, chip_id, chip_index, bank_conf, is_bank_a):
    direction = bank_conf[CONF_DIRECTION]
    inverted = bank_conf[CONF_INVERTED]
    name_prefix = bank_conf.get(CONF_NAME_PREFIX)

    pin_base = 0 if is_bank_a else 8
    for i in range(8):
        pin = pin_base + i
        location = _location_for(bank_conf, chip_index, pin, is_bank_a)
        if direction == DIR_INPUT:
            prefix = name_prefix if name_prefix is not None else DEFAULT_INPUT_NAME_PREFIX
            await _gen_input_pin(
                coord, chip, board_id, chip_id, pin, prefix, location, inverted
            )
        else:
            prefix = name_prefix if name_prefix is not None else DEFAULT_OUTPUT_NAME_PREFIX
            await _gen_output_pin(
                chip, board_id, chip_id, pin, prefix, location, inverted
            )


async def _gen_board(coord, board_conf):
    board_id = board_conf[CONF_BOARD_ID]
    addresses = [board_conf[CONF_CHIP1_ADDRESS], board_conf[CONF_CHIP2_ADDRESS]]

    for chip_index, address in enumerate(addresses):
        chip_id = f"{chip_index + 1:02d}"  # "01", "02"
        chip_var_id = _declare_component_id(
            f"mcp23017_b{board_id}_c{chip_id}", MCP23017Chip
        )
        chip = cg.new_Pvariable(chip_var_id)
        await cg.register_component(chip, {})
        # Register as an I2C device the standard way (sets bus + address).
        await i2c.register_i2c_device(
            chip,
            {CONF_I2C_ID: board_conf[CONF_I2C_ID], CONF_ADDRESS: address},
        )
        cg.add(coord.register_chip(chip))

        await _gen_bank(
            coord, chip, board_id, chip_id, chip_index, board_conf[CONF_BANK_A], True
        )
        await _gen_bank(
            coord, chip, board_id, chip_id, chip_index, board_conf[CONF_BANK_B], False
        )


async def to_code(config):
    coord = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(coord, config)

    interrupt_pin = await cg.gpio_pin_expression(config[CONF_INTERRUPT_PIN])
    cg.add(coord.set_interrupt_pin(interrupt_pin))

    # Low-level chips
    for chip_conf in config.get(CONF_CHIPS, []):
        chip = cg.new_Pvariable(chip_conf[CONF_ID])
        await cg.register_component(chip, chip_conf)
        await i2c.register_i2c_device(chip, chip_conf)
        cg.add(coord.register_chip(chip))

    # High-level boards
    for board_conf in config.get(CONF_BOARDS, []):
        await _gen_board(coord, board_conf)

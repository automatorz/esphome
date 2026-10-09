"""MCP23017 boards config-expansion external component (polling variant).

Python-only convenience / codegen layer. It has NO C++ runtime of its own: in
``to_code`` it programmatically emits STOCK ESPHome entities exactly as if they
had been hand-written:

  * stock ``mcp23017:`` hubs (two per board)
  * stock ``gpio`` ``binary_sensor`` entities for input banks (polling,
    ``use_interrupt: false``)
  * stock ``gpio`` ``output`` entities for output banks, each driving either a
    stock ``switch: platform: output`` (default role) or a stock
    ``light: platform: binary`` (opt-in per pin)

Runtime behaviour is therefore IDENTICAL to writing those stock entities by
hand (polling based, no interrupts). The entire value is replacing a large
hand-written ``!include``-template YAML structure with a compact ``boards:``
block.

This is the POLLING counterpart to ``mcp23017_shared_int`` — it has no
interrupt pin and uses the stock polling data path.

Board config shape::

    mcp23017_boards:
      boards:
        - board_id: "A1"
          i2c_id: bus_a
          chip1_address: 0x20
          chip2_address: 0x21
          bank_a: { direction: input }         # bottom row -> binary_sensors
          bank_b:                              # top row -> outputs
            direction: output
            pins:
              8:
                role: light
                id: workshop_light
                name: "Workshop Light"
                on_buttons: [has_boardA1_chip01_pin00]
                off_buttons: [has_boardA1_chip01_pin01]

A board expands into (for board_id "A1"):
  * chips ``mcp23017_bA1_c01`` and ``..._c02``
  * for each input pin:  binary_sensor id ``has_boardA1_chip<cc>_pin<pp>``,
                         name ``HAS BoardA1 <location> (Chip<cc> Pin<pp>)``
  * for each switch output pin: output id ``has_boardA1_chip<cc>_pin<pp>`` plus
                         switch id ``swA1_chip<cc>_pin<pp>``,
                         name ``ZZHAS BoardA1 <location> (Chip<cc> Pin<pp>)``
  * for each light output pin: output id ``has_boardA1_chip<cc>_pin<pp>`` plus a
                         ``light: platform: binary`` with the user-supplied id
                         and name (no switch — avoids two owners of one output).
"""

from esphome.components import binary_sensor
from esphome.components import i2c
import esphome.config_validation as cv
from esphome.core import CORE, ID
from esphome.const import (
    CONF_DIRECTION,
    CONF_ID,
    CONF_INVERTED,
    CONF_NAME,
    CONF_OUTPUT,
    CONF_RESTORE_MODE,
)

CODEOWNERS = ["@automatorz"]
DEPENDENCIES = ["i2c"]
# AUTO_LOAD every stock platform/component we synthesize so the user does not
# have to declare empty platform blocks just to pull them in.
AUTO_LOAD = ["mcp23017", "binary_sensor", "output", "switch", "light"]
MULTI_CONF = False

# --- config keys ---
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
CONF_PINS = "pins"
CONF_ROLE = "role"
CONF_ON_BUTTONS = "on_buttons"
CONF_OFF_BUTTONS = "off_buttons"

DIR_INPUT = "input"
DIR_OUTPUT = "output"

ROLE_SWITCH = "switch"
ROLE_LIGHT = "light"

# Default Home Assistant name prefixes for auto-generated bank entities.
# The "ZZ" prefix on switch outputs makes them sort last alphabetically in Home
# Assistant, keeping inputs grouped ahead of outputs in entity lists. (Lights
# use a user-supplied, human-meaningful name instead.)
DEFAULT_INPUT_NAME_PREFIX = "HAS"
DEFAULT_OUTPUT_NAME_PREFIX = "ZZHAS"

# Default restore semantics (overridable per pin), matching the hand-written
# template files this component replaces.
DEFAULT_SWITCH_RESTORE_MODE = "ALWAYS_OFF"
DEFAULT_LIGHT_RESTORE_MODE = "RESTORE_DEFAULT_OFF"


# ---------------------------------------------------------------------------
# Per-pin override schema (output banks only)
# ---------------------------------------------------------------------------
_PIN_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_ROLE, default=ROLE_SWITCH): cv.one_of(
            ROLE_SWITCH, ROLE_LIGHT, lower=True
        ),
        # For role switch the id/name default to the standard sw.. / ZZHAS..
        # convention, so both are optional. For role light they are required
        # (a human-meaningful room light name/id) — enforced in _validate_pin.
        cv.Optional(CONF_ID): cv.string,
        cv.Optional(CONF_NAME): cv.string,
        cv.Optional(CONF_RESTORE_MODE): cv.string,
        cv.Optional(CONF_INVERTED): cv.boolean,
        # Light-only: input binary_sensor ids whose on_press wires an additive
        # light.turn_on / light.turn_off automation on this light.
        cv.Optional(CONF_ON_BUTTONS): cv.ensure_list(cv.string),
        cv.Optional(CONF_OFF_BUTTONS): cv.ensure_list(cv.string),
    }
)


def _validate_pin(value):
    value = _PIN_SCHEMA(value)
    role = value[CONF_ROLE]
    if role == ROLE_LIGHT:
        if CONF_ID not in value:
            raise cv.Invalid("A 'light' pin requires an explicit 'id'.")
        if CONF_NAME not in value:
            raise cv.Invalid("A 'light' pin requires an explicit 'name'.")
    else:
        for key in (CONF_ON_BUTTONS, CONF_OFF_BUTTONS):
            if key in value:
                raise cv.Invalid(f"'{key}' is only valid for 'light' pins.")
    return value


# ---------------------------------------------------------------------------
# High-level board / bank schema
# ---------------------------------------------------------------------------
def _bank_schema(default_location_prefix):
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
            # Per-pin role / override map, keyed by physical MCP pin 0..15.
            # Only meaningful for output banks; rejected on inputs below.
            cv.Optional(CONF_PINS): cv.Schema(
                {cv.int_range(min=0, max=15): _validate_pin}
            ),
        }
    )


def _validate_bank(value):
    if value[CONF_DIRECTION] == DIR_INPUT and value.get(CONF_PINS):
        raise cv.Invalid("'pins' overrides are only valid for output banks.")
    return value


BOARD_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_BOARD_ID): cv.string,
        cv.Required(CONF_I2C_ID): cv.use_id(i2c.I2CBus),
        cv.Optional(CONF_CHIP1_ADDRESS, default=0x20): cv.i2c_address,
        cv.Optional(CONF_CHIP2_ADDRESS, default=0x21): cv.i2c_address,
        cv.Required(CONF_BANK_A): cv.All(_bank_schema("Bottom"), _validate_bank),
        cv.Required(CONF_BANK_B): cv.All(_bank_schema("Top"), _validate_bank),
    }
)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_BOARDS): cv.All(
            cv.ensure_list(BOARD_SCHEMA), cv.Length(min=1, max=16)
        ),
    }
)


# ---------------------------------------------------------------------------
# Label generation (identical scheme to the hand-written template files)
# ---------------------------------------------------------------------------
def _default_location(location_prefix, chip_index, pin, is_bank_a):
    """Physical label for a pin.

    Bank A (bottom): position = chip_index*8 + pin + 1       (ascending)
    Bank B (top):    position = chip_index*8 + (16 - pin)    (descending within chip)
    """
    if is_bank_a:
        pos = chip_index * 8 + pin + 1
    else:
        pos = chip_index * 8 + (16 - pin)
    return f"{location_prefix} {pos:02d}"


def _location_for(bank_conf, chip_index, pin, is_bank_a):
    locations = bank_conf.get(CONF_LOCATIONS)
    if is_bank_a:
        pos = chip_index * 8 + pin + 1
    else:
        pos = chip_index * 8 + (16 - pin)
    if locations is not None:
        # Explicit list indexed by physical position (1..16) -> 0-based.
        return locations[pos - 1]
    return _default_location(bank_conf[CONF_LOCATION_PREFIX], chip_index, pin, is_bank_a)


# ---------------------------------------------------------------------------
# Codegen-time id minting
# ---------------------------------------------------------------------------
def _declare_component_id(obj_id, cls):
    """Create a declaration ID for a Component synthesized at codegen time and
    register it in CORE.component_ids so cg.register_component() accepts it.
    (Normally cv.declare_id does this during config validation; our board pins
    are generated after validation, so we register them here.)"""
    the_id = ID(obj_id, is_declaration=True, type=cls)
    CORE.component_ids.add(the_id.id)
    return the_id


def _mcp_pin_config(chip_id_name, pin, inverted, is_input):
    """Build a stock mcp23xxx pin config dict (same shape as the hand-written
    leaf templates). It is run through the stock gpio pin schema by whichever
    platform validator consumes it. ``chip_id_name`` is the hub's id *string*
    so the stock ``use_id`` validator resolves it against the declared hub."""
    if is_input:
        mode = {"input": True, "pullup": True}
    else:
        mode = {"output": True}
    return {
        "mcp23xxx": chip_id_name,
        "number": pin,
        "mode": mode,
        CONF_INVERTED: inverted,
    }


# ---------------------------------------------------------------------------
# Entity generation
# ---------------------------------------------------------------------------
async def _gen_input_pin(
    chip_id_name,
    board_id,
    chip_id,
    pin,
    name_prefix,
    location,
    inverted,
    button_actions,
):
    pin2 = f"{pin:02d}"
    name = f"{name_prefix} Board{board_id} {location} (Chip{chip_id} Pin{pin2})"
    obj_id = f"has_board{board_id}_chip{chip_id}_pin{pin2}"

    conf = {
        CONF_ID: _declare_component_id(obj_id, binary_sensor.BinarySensor),
        CONF_NAME: name,
        "pin": _mcp_pin_config(chip_id_name, pin, inverted, is_input=True),
    }
    # Attach any additive light on_press automations wired via on_buttons /
    # off_buttons on output-bank lights. Using the stock on_press key means the
    # stock binary_sensor machinery builds the triggers, so they coexist with
    # any automations the user adds elsewhere via !extend.
    actions = button_actions.get(obj_id)
    if actions:
        conf["on_press"] = [{"then": [action]} for action in actions]

    # Validate through the stock gpio binary_sensor platform schema, then run
    # its stock to_code so the entity is byte-identical to a hand-written one.
    from esphome.components.gpio.binary_sensor import (
        CONFIG_SCHEMA as GPIO_BS_SCHEMA,
        to_code as gpio_bs_to_code,
    )

    conf = GPIO_BS_SCHEMA(conf)
    await gpio_bs_to_code(conf)


async def _gen_gpio_output(chip_id_name, board_id, chip_id, pin, inverted):
    """Generate the stock gpio BinaryOutput for an output pin and return its id."""
    pin2 = f"{pin:02d}"
    out_id = f"has_board{board_id}_chip{chip_id}_pin{pin2}"

    from esphome.components.gpio.output import (
        GPIOBinaryOutput,
        CONFIG_SCHEMA as GPIO_OUT_SCHEMA,
        to_code as gpio_out_to_code,
    )

    conf = {
        CONF_ID: _declare_component_id(out_id, GPIOBinaryOutput),
        "pin": _mcp_pin_config(chip_id_name, pin, inverted, is_input=False),
    }
    conf = GPIO_OUT_SCHEMA(conf)
    await gpio_out_to_code(conf)
    return out_id


async def _gen_switch_pin(
    chip_id_name, board_id, chip_id, pin, pin_conf, name_prefix, location, bank_inverted
):
    pin2 = f"{pin:02d}"
    inverted = pin_conf.get(CONF_INVERTED, bank_inverted)
    out_id = await _gen_gpio_output(chip_id_name, board_id, chip_id, pin, inverted)

    sw_id = pin_conf.get(CONF_ID, f"sw{board_id}_chip{chip_id}_pin{pin2}")
    name = pin_conf.get(
        CONF_NAME,
        f"{name_prefix} Board{board_id} {location} (Chip{chip_id} Pin{pin2})",
    )
    restore_mode = pin_conf.get(CONF_RESTORE_MODE, DEFAULT_SWITCH_RESTORE_MODE)

    from esphome.components.output.switch import (
        CONFIG_SCHEMA as OUT_SW_SCHEMA,
        to_code as out_sw_to_code,
    )
    from esphome.components.output.switch import OutputSwitch

    conf = {
        CONF_ID: _declare_component_id(sw_id, OutputSwitch),
        CONF_NAME: name,
        CONF_RESTORE_MODE: restore_mode,
        CONF_OUTPUT: out_id,
    }
    conf = OUT_SW_SCHEMA(conf)
    await out_sw_to_code(conf)


async def _gen_light_pin(
    chip_id_name, board_id, chip_id, pin, pin_conf, bank_inverted
):
    inverted = pin_conf.get(CONF_INVERTED, bank_inverted)
    out_id = await _gen_gpio_output(chip_id_name, board_id, chip_id, pin, inverted)

    light_id = pin_conf[CONF_ID]
    name = pin_conf[CONF_NAME]
    restore_mode = pin_conf.get(CONF_RESTORE_MODE, DEFAULT_LIGHT_RESTORE_MODE)

    from esphome.components.binary.light import (
        CONFIG_SCHEMA as BINARY_LIGHT_SCHEMA,
        to_code as binary_light_to_code,
    )
    from esphome.components.light.types import LightState

    conf = {
        CONF_ID: _declare_component_id(light_id, LightState),
        CONF_NAME: name,
        CONF_RESTORE_MODE: restore_mode,
        CONF_OUTPUT: out_id,
    }
    conf = BINARY_LIGHT_SCHEMA(conf)
    await binary_light_to_code(conf)


def _collect_button_actions(bank_conf):
    """Map input binary_sensor id -> list of light.turn_on/turn_off action dicts
    gathered from every light pin's on_buttons / off_buttons in this output bank.
    Multiple lights referencing the same button accumulate (additive)."""
    actions = {}
    for pin_conf in bank_conf.get(CONF_PINS, {}).values():
        if pin_conf[CONF_ROLE] != ROLE_LIGHT:
            continue
        light_id = pin_conf[CONF_ID]
        for btn in pin_conf.get(CONF_ON_BUTTONS, []):
            actions.setdefault(btn, []).append({"light.turn_on": light_id})
        for btn in pin_conf.get(CONF_OFF_BUTTONS, []):
            actions.setdefault(btn, []).append({"light.turn_off": light_id})
    return actions


async def _gen_bank(
    chip_id_name, board_id, chip_id, chip_index, bank_conf, is_bank_a, button_actions
):
    direction = bank_conf[CONF_DIRECTION]
    bank_inverted = bank_conf[CONF_INVERTED]
    name_prefix = bank_conf.get(CONF_NAME_PREFIX)
    pins_conf = bank_conf.get(CONF_PINS, {})

    pin_base = 0 if is_bank_a else 8
    for i in range(8):
        pin = pin_base + i
        location = _location_for(bank_conf, chip_index, pin, is_bank_a)
        if direction == DIR_INPUT:
            prefix = (
                name_prefix if name_prefix is not None else DEFAULT_INPUT_NAME_PREFIX
            )
            await _gen_input_pin(
                chip_id_name,
                board_id,
                chip_id,
                pin,
                prefix,
                location,
                bank_inverted,
                button_actions,
            )
        else:
            prefix = (
                name_prefix if name_prefix is not None else DEFAULT_OUTPUT_NAME_PREFIX
            )
            # Pins without an explicit entry default to the switch role.
            pin_conf = pins_conf.get(pin, {CONF_ROLE: ROLE_SWITCH})
            if pin_conf[CONF_ROLE] == ROLE_LIGHT:
                await _gen_light_pin(
                    chip_id_name, board_id, chip_id, pin, pin_conf, bank_inverted
                )
            else:
                await _gen_switch_pin(
                    chip_id_name,
                    board_id,
                    chip_id,
                    pin,
                    pin_conf,
                    prefix,
                    location,
                    bank_inverted,
                )


async def _gen_board(board_conf):
    board_id = board_conf[CONF_BOARD_ID]
    addresses = [board_conf[CONF_CHIP1_ADDRESS], board_conf[CONF_CHIP2_ADDRESS]]

    # on_buttons / off_buttons on either bank's lights may reference input ids
    # from either chip of this board, so gather them across the whole board
    # before generating any input pins.
    button_actions = {}
    for bank_conf in (board_conf[CONF_BANK_A], board_conf[CONF_BANK_B]):
        if bank_conf[CONF_DIRECTION] == DIR_OUTPUT:
            for btn_id, acts in _collect_button_actions(bank_conf).items():
                button_actions.setdefault(btn_id, []).extend(acts)

    from esphome.components.mcp23017 import (
        CONFIG_SCHEMA as MCP_SCHEMA,
        to_code as mcp_to_code,
    )
    from esphome.components.mcp23017 import MCP23017

    for chip_index, address in enumerate(addresses):
        chip_id = f"{chip_index + 1:02d}"  # "01", "02"
        chip_var_id = _declare_component_id(
            f"mcp23017_b{board_id}_c{chip_id}", MCP23017
        )

        # Generate the stock mcp23017 hub exactly as if hand-written.
        hub_conf = {
            CONF_ID: chip_var_id,
            CONF_I2C_ID: board_conf[CONF_I2C_ID],
            "address": address,
        }
        hub_conf = MCP_SCHEMA(hub_conf)
        await mcp_to_code(hub_conf)

        await _gen_bank(
            chip_var_id.id,
            board_id,
            chip_id,
            chip_index,
            board_conf[CONF_BANK_A],
            True,
            button_actions,
        )
        await _gen_bank(
            chip_var_id.id,
            board_id,
            chip_id,
            chip_index,
            board_conf[CONF_BANK_B],
            False,
            button_actions,
        )


async def to_code(config):
    for board_conf in config[CONF_BOARDS]:
        await _gen_board(board_conf)

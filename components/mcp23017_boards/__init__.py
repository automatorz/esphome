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
            pins:                              # nested: chip -> pin -> override
              "01":                            # chip 1 ("01"/"02" or 1/2)
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


# Canonical two-digit chip identifiers ("01" = chip 1, "02" = chip 2).
_CHIP_IDS = ("01", "02")


def _normalize_chip_key(value):
    """Normalize a user-supplied chip key to the canonical "01"/"02" form.

    Accepts the strings "01"/"02" and the ints 1/2 (and their bare string
    forms "1"/"2"). Raises cv.Invalid for anything else."""
    if isinstance(value, bool):
        # bool is an int subclass; reject it explicitly to avoid True -> "01".
        raise cv.Invalid(f"Invalid chip key {value!r}; expected '01'/'02' or 1/2.")
    if isinstance(value, int):
        text = f"{value:02d}"
    else:
        text = str(value).strip()
        if text in ("1", "2"):
            text = f"0{text}"
    if text not in _CHIP_IDS:
        raise cv.Invalid(
            f"Invalid chip key {value!r}; expected one of '01'/'02' (or 1/2)."
        )
    return text


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
            # Per-pin role / override map, nested by CHIP then physical pin.
            # The chip key is a chip identifier ("01"/"02", or 1/2 which is
            # normalized to "01"/"02" in _validate_bank); the inner key is the
            # physical MCP pin (0..7 for bank_a, 8..15 for bank_b). Only
            # meaningful for output banks; rejected on inputs below.
            cv.Optional(CONF_PINS): cv.Schema(
                {cv.string: cv.Schema({cv.int_: _validate_pin})}
            ),
        }
    )


def _make_validate_bank(is_bank_a):
    """Build the bank-level validator, bound to which physical bank this is so
    the valid pin range is fixed by the bank (not the configurable prefix)."""
    pin_base, pin_bound = (0, 7) if is_bank_a else (8, 15)
    bank_label = "bank_a (Bottom)" if is_bank_a else "bank_b (Top)"

    def _validate_bank(value):
        if value[CONF_DIRECTION] == DIR_INPUT and value.get(CONF_PINS):
            raise cv.Invalid(
                "'pins' overrides are only valid for output banks."
            )

        pins_conf = value.get(CONF_PINS)
        if pins_conf:
            # Re-key the nested pins map by canonical chip id, validate pin
            # ranges, and reject any (chip, pin) collision that only shows up
            # after normalization (e.g. "01"/1 as chip keys, or 8/"8" as pins).
            normalized = {}
            for chip_key, chip_pins in pins_conf.items():
                chip_id = _normalize_chip_key(chip_key)
                if chip_id in normalized:
                    raise cv.Invalid(
                        f"Chip {chip_id} is declared more than once in "
                        f"{bank_label} 'pins' (key {chip_key!r} collides with "
                        f"another chip key after normalization)."
                    )
                seen_pins = {}
                for pin, pin_conf in chip_pins.items():
                    if not pin_base <= pin <= pin_bound:
                        raise cv.Invalid(
                            f"Pin {pin} is out of range for {bank_label} on "
                            f"chip {chip_id}; valid pins are "
                            f"{pin_base}..{pin_bound}."
                        )
                    if pin in seen_pins:
                        raise cv.Invalid(
                            f"Pin {pin} on chip {chip_id} in {bank_label} is "
                            f"declared more than once."
                        )
                    seen_pins[pin] = pin_conf
                normalized[chip_id] = seen_pins
            value[CONF_PINS] = normalized
        return value

    return _validate_bank


BOARD_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_BOARD_ID): cv.string,
        cv.Required(CONF_I2C_ID): cv.use_id(i2c.I2CBus),
        cv.Optional(CONF_CHIP1_ADDRESS, default=0x20): cv.i2c_address,
        cv.Optional(CONF_CHIP2_ADDRESS, default=0x21): cv.i2c_address,
        cv.Required(CONF_BANK_A): cv.All(
            _bank_schema("Bottom"), _make_validate_bank(True)
        ),
        cv.Required(CONF_BANK_B): cv.All(
            _bank_schema("Top"), _make_validate_bank(False)
        ),
    }
)


# ---------------------------------------------------------------------------
# Whole-config cross validation
# ---------------------------------------------------------------------------
def _iter_output_light_pins(boards):
    """Yield (board_id, chip_id, pin, pin_conf) for every light-role output pin
    across the whole config. Runs after per-pin/_bank defaults are applied, so
    pin_conf has its CONF_ROLE/CONF_ID/CONF_NAME populated."""
    for board_conf in boards:
        board_id = board_conf[CONF_BOARD_ID]
        for bank_conf in (board_conf[CONF_BANK_A], board_conf[CONF_BANK_B]):
            if bank_conf[CONF_DIRECTION] != DIR_OUTPUT:
                continue
            for chip_id, chip_pins in bank_conf.get(CONF_PINS, {}).items():
                for pin, pin_conf in chip_pins.items():
                    if pin_conf[CONF_ROLE] == ROLE_LIGHT:
                        yield board_id, chip_id, pin, pin_conf


def _generated_input_ids(boards):
    """Compute the full set of input binary_sensor ids this config generates:
    for every input-direction bank, both chips, every pin in the bank range,
    id ``has_board<BID>_chip<CC>_pin<PP>`` (PP zero-padded), matching
    _gen_input_pin."""
    ids = set()
    for board_conf in boards:
        board_id = board_conf[CONF_BOARD_ID]
        for bank_conf, is_bank_a in (
            (board_conf[CONF_BANK_A], True),
            (board_conf[CONF_BANK_B], False),
        ):
            if bank_conf[CONF_DIRECTION] != DIR_INPUT:
                continue
            pin_base = 0 if is_bank_a else 8
            for chip_id in _CHIP_IDS:
                for i in range(8):
                    pin = pin_base + i
                    ids.add(f"has_board{board_id}_chip{chip_id}_pin{pin:02d}")
    return ids


def _validate_config(config):
    """Whole-config guardrails, applied after all defaults are set:

    1. Duplicate light id / light name across every board/bank/chip.
    2. Every on_buttons/off_buttons id must be an input binary_sensor this
       config actually generates (on either chip of any board)."""
    boards = config[CONF_BOARDS]

    seen_ids = {}
    seen_names = {}
    for board_id, chip_id, pin, pin_conf in _iter_output_light_pins(boards):
        where = f"board {board_id} chip {chip_id} pin {pin}"
        light_id = pin_conf[CONF_ID]
        if light_id in seen_ids:
            raise cv.Invalid(
                f"Duplicate light id '{light_id}': used at {seen_ids[light_id]} "
                f"and {where}. Light ids must be unique across the whole config."
            )
        seen_ids[light_id] = where

        light_name = pin_conf[CONF_NAME]
        if light_name in seen_names:
            raise cv.Invalid(
                f"Duplicate light name '{light_name}': used at "
                f"{seen_names[light_name]} and {where}. Light names must be "
                f"unique across the whole config."
            )
        seen_names[light_name] = where

    valid_input_ids = _generated_input_ids(boards)
    for board_id, chip_id, pin, pin_conf in _iter_output_light_pins(boards):
        light_id = pin_conf[CONF_ID]
        for key in (CONF_ON_BUTTONS, CONF_OFF_BUTTONS):
            for btn in pin_conf.get(key, []):
                if btn not in valid_input_ids:
                    raise cv.Invalid(
                        f"Light '{light_id}' (board {board_id} chip {chip_id} "
                        f"pin {pin}) lists '{btn}' in '{key}', but no input "
                        f"binary_sensor with that id is generated by this "
                        f"config. '{key}' must reference generated input ids "
                        f"(has_board<BID>_chip<CC>_pin<PP>)."
                    )

    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(CONF_BOARDS): cv.All(
                cv.ensure_list(BOARD_SCHEMA), cv.Length(min=1, max=16)
            ),
        }
    ),
    _validate_config,
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
    # The pins map is nested chip -> pin -> pin_conf; iterate the inner dicts.
    for chip_pins in bank_conf.get(CONF_PINS, {}).values():
        for pin_conf in chip_pins.values():
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
            # Look up the override nested under THIS chip, then this pin. A pin
            # with no entry under its chip stays the default switch role, so an
            # override only applies to the chip it is nested under.
            pin_conf = pins_conf.get(chip_id, {}).get(
                pin, {CONF_ROLE: ROLE_SWITCH}
            )
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

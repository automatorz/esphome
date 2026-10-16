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
from esphome.components import binary_sensor, i2c, light, output, switch
import esphome.config_validation as cv
from esphome.core import CORE, ID
from esphome.const import (
    CONF_ADDRESS,
    CONF_DIRECTION,
    CONF_ID,
    CONF_INTERRUPT_PIN,
    CONF_INVERTED,
    CONF_NAME,
    CONF_OUTPUT,
    CONF_OUTPUT_ID,
    CONF_RESTORE_MODE,
)

CODEOWNERS = ["@automatorz"]
DEPENDENCIES = ["i2c"]
AUTO_LOAD = [
    "binary_sensor",
    "switch",
    "output",
    "light",
    # Needed by the embedded shutter_hub (which we do NOT AUTO_LOAD, so its own
    # AUTO_LOAD chain doesn't run): cover entities + the shutter telemetry
    # text_sensor/sensor. Harmless when no shutter_hub block is present.
    "cover",
    "text_sensor",
    "sensor",
]
MULTI_CONF = False

# NOTE: shutter_hub is deliberately NOT in AUTO_LOAD. It is a regular component
# whose schema requires keys (sun_azimuth, etc.), so auto-loading it injects an
# empty top-level `shutter_hub:` section that fails validation. We instead embed
# its config under our own `shutter_hub:` key and, when present, copy its C++
# sources in to_code via _ensure_shutter_hub_sources() (see below).

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
MCP23017Output = mcp23017_shared_int_ns.class_(
    "MCP23017Output", output.BinaryOutput, cg.Component
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
CONF_PINS = "pins"
CONF_ROLE = "role"
CONF_ON_BUTTONS = "on_buttons"
CONF_OFF_BUTTONS = "off_buttons"
CONF_SHUTTER_HUB = "shutter_hub"

DIR_INPUT = "input"
DIR_OUTPUT = "output"

ROLE_SWITCH = "switch"
ROLE_LIGHT = "light"

# Default restore modes (parity with mcp23017_boards).
DEFAULT_SWITCH_RESTORE_MODE = "RESTORE_DEFAULT_OFF"
DEFAULT_LIGHT_RESTORE_MODE = "RESTORE_DEFAULT_OFF"

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
# ---------------------------------------------------------------------------
# Per-pin override schema (output banks only) — parity with mcp23017_boards.
# ---------------------------------------------------------------------------
_PIN_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_ROLE, default=ROLE_SWITCH): cv.one_of(
            ROLE_SWITCH, ROLE_LIGHT, lower=True
        ),
        cv.Optional(CONF_ID): cv.string,
        cv.Optional(CONF_NAME): cv.string,
        cv.Optional(CONF_RESTORE_MODE): cv.string,
        cv.Optional(CONF_INVERTED): cv.boolean,
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


_CHIP_IDS = ("01", "02")


def _normalize_chip_key(value):
    if isinstance(value, bool):
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
            # Per-pin role/override map, nested by CHIP then physical pin.
            # Output banks only (rejected on inputs in _make_validate_bank).
            cv.Optional(CONF_PINS): cv.Schema(
                {cv.string: cv.Schema({cv.int_: _validate_pin})}
            ),
        }
    )


def _make_validate_bank(is_bank_a):
    pin_base, pin_bound = (0, 7) if is_bank_a else (8, 15)
    bank_label = "bank_a (Bottom)" if is_bank_a else "bank_b (Top)"

    def _validate_bank(value):
        if value[CONF_DIRECTION] == DIR_INPUT and value.get(CONF_PINS):
            raise cv.Invalid("'pins' overrides are only valid for output banks.")
        pins_conf = value.get(CONF_PINS)
        if pins_conf:
            normalized = {}
            for chip_key, chip_pins in pins_conf.items():
                chip_id = _normalize_chip_key(chip_key)
                if chip_id in normalized:
                    raise cv.Invalid(
                        f"Chip {chip_id} is declared more than once in "
                        f"{bank_label} 'pins'."
                    )
                seen = {}
                for pin, pin_conf in chip_pins.items():
                    if not pin_base <= pin <= pin_bound:
                        raise cv.Invalid(
                            f"Pin {pin} is out of range for {bank_label} on "
                            f"chip {chip_id}; valid pins are {pin_base}..{pin_bound}."
                        )
                    seen[pin] = pin_conf
                normalized[chip_id] = seen
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
            _bank_schema("Bottom", "HAS"), _make_validate_bank(True)
        ),
        cv.Required(CONF_BANK_B): cv.All(
            _bank_schema("Top", "HAS"), _make_validate_bank(False)
        ),
    }
)


def _validate_at_least_one(config):
    if CONF_CHIPS not in config and CONF_BOARDS not in config:
        raise cv.Invalid(
            f"You must define at least one of '{CONF_CHIPS}' or '{CONF_BOARDS}'."
        )
    return config


def _shutter_hub_schema(value):
    # Lazily reference the sibling component's nested schema so the import only
    # happens when a shutter_hub: block is actually present.
    from esphome.components.shutter_hub import NESTED_CONFIG_SCHEMA

    return NESTED_CONFIG_SCHEMA(value)


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
            # Optional embedded shutter_hub. Uses the shutter_hub component's
            # nested schema (up_relay/down_relay are plain `sw...` name strings
            # resolved against this component's generated switches in codegen).
            cv.Optional(CONF_SHUTTER_HUB): _shutter_hub_schema,
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
    are generated after validation, so we register them here.)

    Adding to CORE.component_ids is idempotent (it's a set), so calling this at
    codegen is safe even if _final_validate already declared the same id during
    validation (see FINAL_VALIDATE_SCHEMA below)."""
    the_id = ID(obj_id, is_declaration=True, type=cls)
    CORE.component_ids.add(the_id.id)
    return the_id


async def _gen_input_pin(
    coord, chip, board_id, chip_id, pin, name_prefix, location, inverted,
    sensor_map=None,
):
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
    # Record this input's var by its auto-generated id so a light's
    # on_buttons/off_buttons can resolve to it after the board loop.
    if sensor_map is not None:
        sensor_map[obj_id] = var
    cg.add(var.set_coordinator(coord))
    cg.add(var.set_chip(chip))
    cg.add(var.set_pin(pin))
    cg.add(var.set_inverted(inverted))
    # Mark the pin as an input on the chip at CODEGEN time, so the chip's
    # input_mask_ is fully populated before any setup() runs. The C++
    # binary_sensor setup() also calls set_pin_input, but relying on that is a
    # setup-ordering trap: the chip's setup() reads input_mask_ to program
    # IODIR/GPINTEN, and if it runs before all its binary_sensors have set up,
    # interrupts are only enabled for a subset of pins (observed: chip only
    # recognized 2 of 8 inputs). Setting it here removes the ordering dependency.
    cg.add(chip.set_pin_input(pin))
    # Register this sensor as a coordinator listener at CODEGEN time (not from
    # the C++ setup()), so listeners_ is deterministically complete before any
    # setup() runs — mirroring the set_pin_input ordering fix above and the
    # existing cg.add(coord.register_chip(chip)) pattern.
    cg.add(coord.register_listener(var))


async def _gen_switch_pin(
    chip, board_id, chip_id, pin, name_prefix, location, bank_inverted, pin_conf,
    switch_map=None,
):
    pin2 = f"{pin:02d}"
    inverted = pin_conf.get(CONF_INVERTED, bank_inverted)
    sw_id = pin_conf.get(CONF_ID, f"sw{board_id}_chip{chip_id}_pin{pin2}")
    name = pin_conf.get(
        CONF_NAME,
        f"{name_prefix} Board{board_id} {location} (Chip{chip_id} Pin{pin2})",
    )
    restore_mode = pin_conf.get(CONF_RESTORE_MODE, DEFAULT_SWITCH_RESTORE_MODE)

    conf = {
        CONF_ID: _declare_component_id(sw_id, MCP23017Switch),
        CONF_NAME: name,
        CONF_INVERTED: inverted,
    }
    conf = switch.switch_schema(MCP23017Switch, default_restore_mode=restore_mode)(conf)

    var = await switch.new_switch(conf)
    await cg.register_component(var, conf)
    cg.add(var.set_chip(chip))
    cg.add(var.set_pin(pin))
    # Explicitly mark as output at codegen time (chip defaults to output, but be
    # explicit so input_mask_ is unambiguous regardless of setup ordering).
    cg.add(chip.set_pin_output(pin))
    # Tell the chip this output pin's OFF hardware level so configure_hardware()
    # pre-loads OLAT and the relay doesn't glitch ON at boot. For an inverted
    # switch, logical-off = hardware-high.
    cg.add(chip.set_output_off_level(pin, inverted))
    # Record the switch var by its name so a nested shutter_hub can resolve
    # up_relay/down_relay references (e.g. "sw02_chip01_pin15") to this var.
    if switch_map is not None:
        switch_map[sw_id] = var


async def _gen_light_pin(
    chip, board_id, chip_id, pin, bank_inverted, pin_conf, light_map=None
):
    """Generate an MCP23017Output and wrap it in a stock binary light.

    Uses our own MCP23017Output (an output::BinaryOutput that writes via the
    hub), so there is no dependency on stock gpio platform sources — only the
    stock `binary` light + `light`/`output` domains, which we AUTO_LOAD.
    """
    pin2 = f"{pin:02d}"
    inverted = pin_conf.get(CONF_INVERTED, bank_inverted)
    light_id = pin_conf[CONF_ID]
    name = pin_conf[CONF_NAME]
    restore_mode = pin_conf.get(CONF_RESTORE_MODE, DEFAULT_LIGHT_RESTORE_MODE)

    out_id = f"{light_id}_mcpout"
    out_var = cg.new_Pvariable(_declare_component_id(out_id, MCP23017Output))
    await cg.register_component(out_var, {})
    cg.add(out_var.set_chip(chip))
    cg.add(out_var.set_pin(pin))
    cg.add(out_var.set_inverted(inverted))
    # Mark as output on the chip at codegen time (ordering-independent).
    cg.add(chip.set_pin_output(pin))
    # Pre-load OLAT off-level so the light output doesn't glitch on at boot.
    cg.add(chip.set_output_off_level(pin, inverted))

    from esphome.components.binary.light import (
        BinaryLightOutput,
        CONFIG_SCHEMA as BINARY_LIGHT_SCHEMA,
        to_code as binary_light_to_code,
    )
    from esphome.components.light.types import LightState

    light_var_id = _declare_component_id(light_id, LightState)
    binary_out_id = _declare_component_id(f"{light_id}_binaryout", BinaryLightOutput)

    conf = {
        CONF_OUTPUT_ID: binary_out_id,
        CONF_ID: light_var_id,
        CONF_NAME: name,
        CONF_RESTORE_MODE: restore_mode,
        CONF_OUTPUT: out_id,
    }
    conf = BINARY_LIGHT_SCHEMA(conf)
    # The binary light's to_code resolves CONF_OUTPUT via get_variable; our
    # out_var is already registered under out_id so it resolves cleanly.
    await binary_light_to_code(conf)

    # Record the generated LightState var so on_buttons/off_buttons can be wired
    # to it after the board loop (the button sensors may not exist yet here).
    # binary_light_to_code declared it under light_var_id; retrieve the var.
    if light_map is not None:
        light_var = await cg.get_variable(light_var_id)
        light_map[light_id] = light_var


def _collect_button_links(pin_conf, button_links):
    """Append (light_id, light_name, button_id, is_on) rows for one light pin's
    on_buttons/off_buttons. A single button MAY drive multiple lights in any mix
    of on/off (e.g. one wall button turns one light on and another off), so no
    one-button-one-light restriction is imposed. Resolution to actual sensor
    vars happens after the board loop; the C++ side accumulates the actions."""
    light_id = pin_conf[CONF_ID]
    light_name = pin_conf[CONF_NAME]
    for key, is_on in ((CONF_ON_BUTTONS, True), (CONF_OFF_BUTTONS, False)):
        for button_id in pin_conf.get(key, []):
            button_links.append((light_id, light_name, button_id, is_on))


async def _gen_bank(
    coord, chip, board_id, chip_id, chip_index, bank_conf, is_bank_a,
    switch_map=None, sensor_map=None, light_map=None, button_links=None,
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
            prefix = name_prefix if name_prefix is not None else DEFAULT_INPUT_NAME_PREFIX
            await _gen_input_pin(
                coord, chip, board_id, chip_id, pin, prefix, location, bank_inverted,
                sensor_map=sensor_map,
            )
        else:
            prefix = name_prefix if name_prefix is not None else DEFAULT_OUTPUT_NAME_PREFIX
            # Per-pin override (nested chip -> pin). No entry -> default switch.
            pin_conf = pins_conf.get(chip_id, {}).get(pin, {CONF_ROLE: ROLE_SWITCH})
            if pin_conf[CONF_ROLE] == ROLE_LIGHT:
                await _gen_light_pin(
                    chip, board_id, chip_id, pin, bank_inverted, pin_conf,
                    light_map=light_map,
                )
                # Collect the button->light intent now; resolve after the loop.
                if button_links is not None:
                    _collect_button_links(pin_conf, button_links)
            else:
                await _gen_switch_pin(
                    chip, board_id, chip_id, pin, prefix, location, bank_inverted,
                    pin_conf, switch_map=switch_map,
                )


async def _gen_board(
    coord, board_conf, switch_map=None, sensor_map=None, light_map=None,
    button_links=None,
):
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
            coord, chip, board_id, chip_id, chip_index, board_conf[CONF_BANK_A], True,
            switch_map=switch_map, sensor_map=sensor_map, light_map=light_map,
            button_links=button_links,
        )
        await _gen_bank(
            coord, chip, board_id, chip_id, chip_index, board_conf[CONF_BANK_B], False,
            switch_map=switch_map, sensor_map=sensor_map, light_map=light_map,
            button_links=button_links,
        )


def _ensure_binary_light_sources():
    """Ensure the stock `binary` light platform's C++ sources are copied.

    Light-role pins wrap our MCP23017Output in a stock `light: platform: binary`
    (binary::BinaryLightOutput). esphome copies a platform's sources only when a
    `{platform: X}` entry exists under that domain in CORE.config (see
    config.iter_components). We generate the light via to_code directly, so no
    such entry exists and binary's header is never copied ("'binary' has not
    been declared"). Inject a minimal marker entry so the writer copies it.
    Only needed when at least one light pin is generated; harmless otherwise.
    """
    from esphome.const import CONF_PLATFORM

    entries = CORE.config.setdefault("light", [])
    if not any(
        isinstance(e, dict) and e.get(CONF_PLATFORM) == "binary" for e in entries
    ):
        entries.append({CONF_PLATFORM: "binary"})
    CORE.loaded_integrations.add("light")


def _config_has_light(config):
    for board_conf in config.get(CONF_BOARDS, []):
        for bank in (board_conf[CONF_BANK_A], board_conf[CONF_BANK_B]):
            for chip_pins in bank.get(CONF_PINS, {}).values():
                for pin_conf in chip_pins.values():
                    if pin_conf.get(CONF_ROLE) == ROLE_LIGHT:
                        return True
    return False


# ---------------------------------------------------------------------------
# Root-cause fix (B1): declare fabricated component IDs during VALIDATION.
#
# ESPHome sizes its fixed component array from ESPHOME_COMPONENT_COUNT =
# len(CORE.component_ids), captured during the core codegen stage
# (@coroutine_with_priority(CoroPriority.CORE) in esphome/core/config.py). This
# component fabricates ~130 components (board chips plus per-pin binary sensors,
# switches, outputs and light helpers) during its OWN codegen, which runs at the
# default priority (CoroPriority.COMPONENT) AFTER the count was already frozen.
# Those IDs were therefore never counted, the array was undersized, and
# StaticVector::push_back silently dropped every component past the capacity so
# their setup()/loop() never ran.
#
# FINAL_VALIDATE_SCHEMA runs during config validation, i.e. before ANY codegen
# stage. By declaring the IDs we will fabricate here, they are present in
# CORE.component_ids when the core stage computes the count, so the array is
# sized to fit them. At codegen, _declare_component_id adds the same IDs again
# (idempotent on a set) and cg.register_component removes them as usual — all of
# which happens after the count was taken, so the count stays correct.
# ---------------------------------------------------------------------------
def _fabricated_component_ids(config):
    """Yield every component ID this integration fabricates at codegen time, in
    the SAME form _gen_board/_gen_bank/_gen_*_pin produce them. Keep this in
    lockstep with those generators."""
    for board_conf in config.get(CONF_BOARDS, []):
        board_id = board_conf[CONF_BOARD_ID]
        for chip_index in range(2):
            chip_id = f"{chip_index + 1:02d}"  # "01", "02"
            # The chip component itself.
            yield f"mcp23017_b{board_id}_c{chip_id}"
            for is_bank_a, bank_conf in (
                (True, board_conf[CONF_BANK_A]),
                (False, board_conf[CONF_BANK_B]),
            ):
                direction = bank_conf[CONF_DIRECTION]
                pins_conf = bank_conf.get(CONF_PINS, {})
                pin_base = 0 if is_bank_a else 8
                for i in range(8):
                    pin = pin_base + i
                    pin2 = f"{pin:02d}"
                    if direction == DIR_INPUT:
                        yield f"has_board{board_id}_chip{chip_id}_pin{pin2}"
                    else:
                        pin_conf = pins_conf.get(chip_id, {}).get(
                            pin, {CONF_ROLE: ROLE_SWITCH}
                        )
                        if pin_conf[CONF_ROLE] == ROLE_LIGHT:
                            light_id = pin_conf[CONF_ID]
                            # Mirrors _gen_light_pin: output + light state +
                            # binary-light output wrapper.
                            yield f"{light_id}_mcpout"
                            yield light_id
                            yield f"{light_id}_binaryout"
                        else:
                            yield pin_conf.get(
                                CONF_ID,
                                f"sw{board_id}_chip{chip_id}_pin{pin2}",
                            )


def _fabricated_shutter_hub_ids(config):
    """Yield the component IDs the embedded shutter_hub fabricates at codegen but
    does NOT declare during validation: the per-shutter telemetry text_sensor /
    sensor entities (built in shutter_hub.build_shutter_hub via _declare_entity_id).

    The hub, shutters and groups themselves are declared by the nested
    shutter_hub schema during validation, so they are already counted; only the
    telemetry entities are created purely at codegen. The exact collision-resolved
    name is produced at codegen; here we only need the COUNT to include them, so
    we emit a deterministic base name per enabled telemetry entity."""
    from esphome.helpers import sanitize, snake_case

    sh = config.get(CONF_SHUTTER_HUB)
    if not sh:
        return
    # Keys match shutter_hub's schema (CONF_SHUTTERS/CONF_STATUS_SENSOR/
    # CONF_POSITION_SENSOR there). Both telemetry flags default True.
    for shutter in sh.get("shutters", []):
        base_name = shutter.get(CONF_NAME) or str(shutter[CONF_ID])
        if shutter.get("status_sensor", True):
            yield sanitize(snake_case(f"{base_name}_status"))
        if shutter.get("position_sensor", True):
            yield sanitize(snake_case(f"{base_name}_position"))


def _final_validate(config):
    # Declare every component ID this integration (and its embedded shutter_hub)
    # fabricates at codegen, so CORE.component_ids reflects them before the core
    # stage freezes ESPHOME_COMPONENT_COUNT. See the comment block above.
    for obj_id in _fabricated_component_ids(config):
        CORE.component_ids.add(obj_id)
    for obj_id in _fabricated_shutter_hub_ids(config):
        CORE.component_ids.add(obj_id)
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    if _config_has_light(config):
        _ensure_binary_light_sources()

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

    # High-level boards. Accumulate a name -> switch var map so a nested
    # shutter_hub can resolve up_relay/down_relay references ("sw...") to the
    # actual generated MCP23017Switch vars (no cross-component use_id needed).
    #
    # Also accumulate, mirroring switch_map:
    #   sensor_map    : input id ("has_board..._pin..") -> MCP23017BinarySensor var
    #   light_map     : light id -> light::LightState var
    #   button_links  : (light_id, light_name, button_id, is_on) rows gathered
    #                   from each light pin's on_buttons/off_buttons.
    # These are resolved/wired AFTER the board loop (below), when every input
    # sensor and light var exists (a light's buttons may live on another board).
    #
    # NOTE (Approach B): button->light wiring is done purely via C++ setters on
    # the already-declared MCP23017BinarySensor vars (set_light_on/off). It
    # fabricates NO new Component-derived objects and NO new declared ids, so
    # ESPHOME_COMPONENT_COUNT and FINAL_VALIDATE_SCHEMA/_fabricated_component_ids
    # need no changes.
    switch_map = {}
    sensor_map = {}
    light_map = {}
    button_links = []
    for board_conf in config.get(CONF_BOARDS, []):
        await _gen_board(
            coord, board_conf, switch_map=switch_map, sensor_map=sensor_map,
            light_map=light_map, button_links=button_links,
        )

    # Wire each light's on_buttons/off_buttons now that all input sensors and
    # light vars exist. Resolve the button id against sensor_map and emit the
    # C++ setter binding it to the light's LightState var.
    for light_id, light_name, button_id, is_on in button_links:
        sensor_var = sensor_map.get(button_id)
        if sensor_var is None:
            raise cv.Invalid(
                f"Light '{light_id}' ({light_name}) references button "
                f"'{button_id}' in its "
                f"{'on_buttons' if is_on else 'off_buttons'}, but no input with "
                f"that id was generated. It must be an auto-generated input id "
                f"of the form 'has_board<bb>_chip<cc>_pin<pp>'."
            )
        light_var = light_map[light_id]
        if is_on:
            cg.add(sensor_var.set_light_on(light_var))
        else:
            cg.add(sensor_var.set_light_off(light_var))

    # Embedded shutter_hub (optional). Boards are already expanded above, so the
    # switch_map is fully populated before we wire the shutters.
    if CONF_SHUTTER_HUB in config:
        # Ensure shutter_hub's C++ sources get copied into the build. Its
        # entities are generated by build_shutter_hub below (not by a top-level
        # shutter_hub: section), so it is not otherwise in CORE.config. Adding a
        # sentinel key here (during codegen, after validation) makes the writer
        # copy its resources without running shutter_hub's own schema/to_code.
        if "shutter_hub" not in CORE.config:
            CORE.config["shutter_hub"] = []
        CORE.loaded_integrations.add("shutter_hub")

        from esphome.components.shutter_hub import build_shutter_hub

        async def _resolve_relay(ref):
            if ref not in switch_map:
                raise cv.Invalid(
                    f"shutter_hub relay '{ref}' does not match any generated "
                    f"output switch. It must be the id of an output-direction "
                    f"pin generated by this component's boards (e.g. "
                    f"'sw<board>_chip<cc>_pin<pp>')."
                )
            return switch_map[ref]

        await build_shutter_hub(
            config[CONF_SHUTTER_HUB], relay_resolver=_resolve_relay
        )

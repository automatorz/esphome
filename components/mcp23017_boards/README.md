# mcp23017_boards

A Python-only **config-expansion** external component for ESPHome. It has no
C++ runtime of its own. During code generation it emits plain **stock** ESPHome
entities — `mcp23017` hubs, `gpio` binary_sensors, `gpio` outputs, `output`
switches and `binary` lights — exactly as if you had written them by hand.

The entire value is ergonomics: a compact `boards:` block replaces a large
hand-written structure of `!include`-template YAML files. Runtime behaviour is
**identical** to the stock entities it generates (polling based, no interrupts).

This is the **polling** counterpart to `mcp23017_shared_int`. It has no
interrupt pin and uses the stock polling data path. It does not change I2C
runtime behaviour in any way.

## What it generates

A "board" is two MCP23017 chips on one I2C bus. Each board has two banks:

- `bank_a` → pins 0–7 of each chip (the physical **Bottom** row)
- `bank_b` → pins 8–15 of each chip (the physical **Top** row)

For `board_id: "A1"` a board expands into:

- Two hubs: `mcp23017_bA1_c01` and `mcp23017_bA1_c02`.
- **Input bank** → 16 stock `gpio` binary_sensors:
  - id `has_boardA1_chip<CC>_pin<PP>`
  - name `HAS BoardA1 <location> (Chip<CC> Pin<PP>)`
  - polling (`use_interrupt: false`), `inverted: true`, mode input + pullup
- **Output bank**, per pin, depending on its role:
  - `switch` role (default): a `gpio` output `has_boardA1_chip<CC>_pin<PP>`
    plus an `output` switch `swA1_chip<CC>_pin<PP>`, name
    `ZZHAS BoardA1 <location> (Chip<CC> Pin<PP>)`, `restore_mode: ALWAYS_OFF`.
  - `light` role: the same `gpio` output plus a `binary` light using your own
    `id`/`name` (**no** switch — avoids two owners of one output),
    `restore_mode: RESTORE_DEFAULT_OFF`.

`<CC>` is the two-digit chip number (`01`/`02`), `<PP>` the two-digit physical
MCP pin (`00`..`15`).

### Location labels

Labels match the hand-written template scheme:

- Bank A (Bottom): ascending across the two chips — chip 1 pins 0..7 →
  `Bottom 01`..`Bottom 08`, chip 2 pins 0..7 → `Bottom 09`..`Bottom 16`.
- Bank B (Top): descending within each chip — chip 1 pins 8..15 →
  `Top 08`..`Top 01`, chip 2 pins 8..15 → `Top 16`..`Top 09`.

## Configuration

```yaml
mcp23017_boards:
  boards:
    - board_id: "A1"            # required, string; used in generated ids/names
      i2c_id: bus_a             # required, id of an i2c bus
      chip1_address: 0x20       # optional, default 0x20
      chip2_address: 0x21       # optional, default 0x21
      bank_a: { direction: input }
      bank_b:
        direction: output
        pins:                     # nested: chip -> pin -> override
          "01":                   # chip 1
            8:
              role: light
              id: workshop_light
              name: "Workshop Light"
              on_buttons: [has_boardA1_chip01_pin00]
              off_buttons: [has_boardA1_chip01_pin01]
            9:
              role: switch        # same as omitting it (switch is the default)
          "02":                   # chip 2
            8:
              role: light
              id: hallway_light
              name: "Hallway Light"
```

### Board keys

| Key              | Required | Default | Description                              |
| ---------------- | -------- | ------- | ---------------------------------------- |
| `board_id`       | yes      | —       | String used in all generated ids/names.  |
| `i2c_id`         | yes      | —       | Id of the `i2c` bus the chips sit on.    |
| `chip1_address`  | no       | `0x20`  | I2C address of chip 1.                   |
| `chip2_address`  | no       | `0x21`  | I2C address of chip 2.                   |
| `bank_a`         | yes      | —       | Bank for pins 0–7 (Bottom row).          |
| `bank_b`         | yes      | —       | Bank for pins 8–15 (Top row).            |

### Bank keys

| Key               | Required | Default                     | Description                                            |
| ----------------- | -------- | --------------------------- | ------------------------------------------------------ |
| `direction`       | yes      | —                           | `input` or `output`.                                   |
| `inverted`        | no       | `true`                      | Pin inversion for every pin in the bank.               |
| `name_prefix`     | no       | `HAS` / `ZZHAS`             | HA name prefix (input / switch output).                |
| `location_prefix` | no       | `Bottom` (A) / `Top` (B)    | Prefix for auto-generated location labels.             |
| `locations`       | no       | auto                        | Explicit 16-entry label list (physical position 1–16). |
| `pins`            | no       | `{}`                        | Per-pin override map (output banks only).              |

### Per-pin keys (output banks)

The `pins:` map is **nested by chip, then by physical pin**:

```yaml
pins:
  "01":          # chip 1
    8: { role: light, id: workshop_light, name: "Workshop Light" }
    9: { role: switch }
  "02":          # chip 2
    8: { role: switch }
```

- **Chip key** (outer): a chip identifier. Accepted forms are the strings
  `"01"` / `"02"` and the bare ints `1` / `2` (and `"1"` / `"2"`); all normalize
  to the canonical `"01"` / `"02"`. Declaring the same chip twice under
  different key forms (e.g. `"01"` and `1`) is an error.
- **Pin key** (inner): the physical MCP pin. The valid range depends on the
  bank: `0`–`7` for `bank_a`, `8`–`15` for `bank_b`. An out-of-range pin is
  rejected with an error naming the pin, chip, and bank.

An override only applies to the chip it is nested under — a `light` on chip
`"01"` pin 8 does **not** also create one on chip `"02"` pin 8. Any pin not
listed under its chip defaults to the `switch` role, so you only need an entry
for pins that are lights or that need non-default settings.

| Key            | Role     | Default                                   | Description                                     |
| -------------- | -------- | ----------------------------------------- | ----------------------------------------------- |
| `role`         | both     | `switch`                                  | `switch` or `light`.                            |
| `id`           | both     | `sw<id>_chip<CC>_pin<PP>` (switch)        | Entity id. **Required** for `light`.            |
| `name`         | both     | `ZZHAS ...` auto-name (switch)            | HA name. **Required** for `light`.              |
| `restore_mode` | both     | `ALWAYS_OFF` (sw) / `RESTORE_DEFAULT_OFF` (light) | Restore behaviour.                      |
| `inverted`     | both     | inherits bank `inverted`                  | Per-pin inversion override.                     |
| `on_buttons`   | light    | —                                         | List of input binary_sensor ids → `light.turn_on`.  |
| `off_buttons`  | light    | —                                         | List of input binary_sensor ids → `light.turn_off`. |

### Defaults

All defaults match the configuration this component replaces, and each is
overridable through the same config key:

- inputs: `inverted: true`, pullup on, `use_interrupt: false`
- switch outputs: `inverted: true`, `restore_mode: ALWAYS_OFF`
- light outputs: `inverted: true`, `restore_mode: RESTORE_DEFAULT_OFF`

### `on_buttons` / `off_buttons`

For a `light` pin, each id in `on_buttons` gets an additive
`on_press: - light.turn_on: <this light>` automation, and each id in
`off_buttons` an `on_press: - light.turn_off: <this light>`. These reference the
generated input ids (`has_board<id>_chip<CC>_pin<PP>`). They are **additive** —
multiple lights can share a button, and you can attach further automations
elsewhere.

## Validation guarantees

The component validates the whole `boards:` config before any code is
generated, so a mistake fails fast with a clear message instead of producing a
broken ESPHome config:

1. **No duplicate output pin.** Each `(chip, pin)` output override may be
   declared only once. Keys that collide after normalization (e.g. the chip
   keys `"01"` and `1`) are rejected.
2. **No duplicate light id or name.** Every `light` pin's `id` and `name` must
   be unique across the entire config — across all boards, banks and chips. A
   collision names the offending id/name and both locations.
3. **Button references must exist.** Every id in a light's `on_buttons` /
   `off_buttons` must be an input binary_sensor this config actually generates
   (`has_board<BID>_chip<CC>_pin<PP>`, for a pin in an `input` bank). Buttons
   may reference inputs on either chip of any board; an unknown id is rejected
   with a message naming the id and the light.

## Overriding generated entities with `!extend`

Because the generated entities are real, declared stock entities, you can target
them by id with `!extend` to add behaviour without re-declaring them:

```yaml
binary_sensor:
  - id: !extend has_boardA1_chip01_pin07
    on_press:
      - switch.toggle: swA1_chip02_pin08
```

The generated `has_...` input ids and `sw...` switch ids are stable, so existing
`!extend` overrides keyed on them keep working.

## Notes

- `AUTO_LOAD` pulls in the `mcp23017`, `binary_sensor`, `output`, `switch` and
  `light` stock components, so you do not need to declare them yourself.
- See `example.yaml` for a complete, didactic example.

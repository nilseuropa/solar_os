+++
id = "input.keymap"
title = "Source keymaps"
section = "hardware"
summary = "Inspect and customize registered input-source mappings"
aliases = ["keymap", "input-keymap", "keyboard-mapping"]
keywords = "input keyboard keymap physical matrix modifiers layers symbols tap hold fn caps control json inputronic tca8418 pager"
packages_any = ["service_input_keymap"]
agent_reference_sections = true
+++
# Source keymaps

A source keymap translates physical keys into HID usages, modifiers, layers,
and logical key bytes before the common input service handles repeat and
routing. Each registered source starts with its built-in profile and maintains
its own modifier, Caps, and layer state.

Source mapping and keyboard language layout are separate settings.
`setterm keyboard us|de` selects how HID usages become characters. Its
`setterm keymap` alias selects the same language layout; it does not load a
source mapping file.

## Discover and control mappings

```text
input keymap
input keymap keyboard0 show
input keymap keyboard0 load /keymap.json
input keymap keyboard0 reset
```

The first command lists registered input sources and their mapping support.
`show` reports physical IDs, optional matrix coordinates, and both mapping
layers. Omitting `show` also displays the map. `load` validates a file before
applying it; `reset` restores the source's built-in profile.

`tca8418`, `inputronic-keyboard`, and `lilygo-pager-keyboard` register physical
mapping profiles with modifier, layer, and tap/hold support. Other drivers
currently appear as `unsupported` until they register a profile. A character
tap alone does not identify a physical key or provide a real held state.

## Mapping files

Files are JSON objects of at most 16 KiB, with `"schema": 1` and a `keys`
array, a `symbols` array, or both. Each load begins with the built-in map and
replaces the listed entries. Sources can register up to 256 physical IDs.
The IDs are source-local integers from 1 through 65535; they need not be
consecutive or match HID usages. Use the IDs reported by `show`.

For example, on Inputronic, physical ID 52 is the A position. This maps it to
the B HID usage:

```json
{"schema": 1, "keys": [{"physical": 52, "usage": 5}]}
```

A source with matrix metadata also accepts zero-based `row` and `col`
selectors in place of `physical`. An entry must use exactly one selector form;
two entries that address the same physical key are rejected even if their
selector forms differ. Matrix coordinates are converted using the source's
registered first ID and row stride. TCA8418 profiles use
`physical = 1 + row * 10 + col`, including matrices narrower than ten columns.
Mapping files cannot alter the set of physical IDs or the matrix geometry.

For a generic TCA8418 reference map, this changes Q to A and makes Space a
symbol selector when held; Space+A emits an exclamation mark:

```json
{
  "schema": 1,
  "keys": [
    {"row": 0, "col": 0, "usage": 4},
    {"row": 3, "col": 0, "usage": 44, "layer_tap": true}
  ],
  "symbols": [{"row": 0, "col": 0, "key": 33}]
}
```

`keys` defines base entries; `symbols` defines overrides while the layer
selector is held. Unlisted entries retain their built-in mappings. Positions
without a symbol mapping use the base layer. Optional output fields are:

- `usage`: canonical USB HID keyboard usage, expressed as a decimal integer.
- `key` and `shift_key`: SolarOS logical key bytes, 0 through 255. Nonzero
  values override character translation; Control/Alt chords use the HID usage.
- `raw: true`: publish physical press/release events without a logical key.
- `layer_tap: true`: hold to select symbols; an unused tap emits the specified
  `usage` or `key` on release. Only one base-layer selector is allowed.
- `alt_block: true`: suppress logical output when pressed with Alt. The
  Pager profile uses this for its Alt+B reservation.

An entry with only a selector clears that entry in the selected layer. An
empty symbol entry falls back to the base layer. Raw entries cannot specify
logical output. Duplicate keys, unknown fields, invalid usages, and unknown
physical IDs are rejected without changing the active map.

A successful apply releases held keys, clears queued controller events, and
resets modifier, Caps, and layer state. Release and press any still-held keys
again. If hardware preparation fails, the previous map stays active while
the driver recovers; held keys have been released.

Mappings last until detach or reboot. Load a saved file from a startup script
to reuse it after startup.

## Desktop keymap tool

From a SolarOS source checkout, use the Python CLI to create, edit, inspect,
or validate a mapping. It uses only the Python standard library.

```sh
python3 scripts/keymap.py profiles
python3 scripts/keymap.py show --profile inputronic-keyboard
python3 scripts/keymap.py create --profile inputronic-keyboard \
  --set FN1=F1 --set FN2=Escape --output my-keymap.json
python3 scripts/keymap.py validate --profile inputronic-keyboard --input my-keymap.json
python3 scripts/keymap.py edit --profile inputronic-keyboard \
  --input my-keymap.json --set FN3=Tab
```

`create --interactive` prompts for the six Inputronic FN keys. On the other
profiles it accepts named assignments until an empty line finishes. An
interactive terminal also enters this mode when no assignments are supplied.
`show` lists the profile's key names, physical IDs, and both layers; add
`--input PATH` to preview a mapping. Editing retains existing assignments.
Creation requires `--force` to replace an existing output file. Editing updates
its input file unless a separate `--output PATH` is supplied.

Assignment names are case-insensitive. Use a key name from `show` or
`physical:ID`, for example `--set physical:78=F1`. Actions include HID names
such as `F1`, `Escape`, `Ctrl`, `Shift`, and `Space`; `char:!` for a logical
byte; `usage:0x3a` or `key:27` for numeric output; `raw` for physical events;
and `none` to clear an entry. `tap:Space` selects the symbol layer when held
and emits Space on an unused tap.

For a generic TCA8418 with a smaller matrix, add `--rows N --cols N` to
match its configured geometry. Physical IDs retain the controller's
ten-column stride; keys outside the selected geometry are rejected. The
Inputronic and Pager profiles have fixed geometry.

Use `--symbol KEY=ACTION` to change a symbol entry. For example:

```sh
python3 scripts/keymap.py create --profile inputronic-keyboard \
  --set FN3=tap:Space --symbol A=char:! --output symbols.json
```

Only one layer selector is permitted. The Pager already uses Space as its
selector; replace that assignment before choosing a different selector.
The tool writes ordinary schema-1 JSON containing only selected overrides.
Explicit assignments are retained even when they match the standard profile.
Profiles describe the standard mappings, so previews do not automatically
include custom defaults compiled into a particular firmware image.

## Compile custom defaults

Select a keyboard profile and a mapping file when building firmware:

```sh
SOLAR_OS_KEYMAP_PROFILE=inputronic-keyboard \
SOLAR_OS_KEYMAP_FILE=my-keymap.json \
pio run -e waveshare_esp32_s3_sim7670g_4g
```

The supported profiles are `inputronic-keyboard`, `lilygo-pager-keyboard`,
and `tca8418`. Profile definitions in `keymaps/profiles/` supply shared key
names and standard mappings to the desktop tool and firmware generator.
Custom files replace their listed entries in the standard profile. The
resulting tables are compiled into firmware; a device-side JSON file is not
needed at startup. The selection applies to all attachments of that profile.
For a generic TCA8418 configured with fewer rows or columns, only its active
matrix positions are used.

Both variables must be supplied together. Relative paths are resolved from
the SolarOS project directory. The same selections can be placed in a
PlatformIO environment as `custom_solaros_keymap_profile` and
`custom_solaros_keymap_file`, or supplied as CMake definitions with the
`SOLAR_OS_KEYMAP_PROFILE` and `SOLAR_OS_KEYMAP_FILE` names.

For several custom profiles, set `SOLAR_OS_KEYMAPS_FILE` to an index file:

```json
{
  "schema": 1,
  "profiles": {
    "inputronic-keyboard": "inputronic.json",
    "lilygo-pager-keyboard": "pager.json"
  }
}
```

Paths within the index are relative to the index's directory. Use
`SOLAR_OS_KEYMAPS_FILE` by itself; it cannot be combined with the single-profile
variables. Its PlatformIO option is `custom_solaros_keymaps_file`. Every
selected profile's package must be enabled in the build. Missing files,
unknown profiles, and invalid maps stop the build. File edits trigger
regeneration; removing the selection restores the standard tables on rebuild.

Boot and `input keymap SOURCE reset` use the compiled default, including
custom assignments. Runtime loads also start from that compiled default, so
omitted entries retain it. To return to the standard firmware default, rebuild
without the custom selection. This does not alter the selected US/DE layout.

## Python and Lua

When `service.input-keymap` is included, both runtimes provide:

- `solaros.input.keymap_info(name)` returns mapping capability information.
- `solaros.input.load_keymap(name, path)` loads a validated file.
- `solaros.input.reset_keymap(name)` restores the built-in profile.

`keymap_info` returns a dictionary in Python and a table in Lua. Fields are
`source`, `capabilities`, `key_count`, `rows`, `cols`, `first`, `stride`, and
boolean `supported`, `physical`, `modifiers`, `layers`, and `tap_hold`.
Unsupported sources have `supported=false`; non-matrix profiles have zero
rows and columns. An unknown source raises an error. Load/reset return `None`
in Python and no values in Lua; failures raise an exception or error.

## Quick reference

- `input keymap` lists source mapping support.
- `input keymap SOURCE [show|load PATH|reset]` controls a registered profile.
- Files require `"schema":1` and `keys` or `symbols`; each entry selects
  `physical` or both `row` and `col`, with optional `usage`, `key`,
  `shift_key`, `raw`, `layer_tap`, or `alt_block` output fields.
- `solaros.input.keymap_info(name)` reports supported features.
- `solaros.input.load_keymap(name,path)` and `reset_keymap(name)` apply changes.
- Mappings last until detach or reboot. Release and press held keys again
  after applying a map. `setterm keyboard us|de` selects language layout.
- `python3 scripts/keymap.py create --profile PROFILE --set KEY=ACTION --output PATH`
  creates a sparse mapping from a desktop source checkout.
- `SOLAR_OS_KEYMAP_PROFILE` with `SOLAR_OS_KEYMAP_FILE`, or `SOLAR_OS_KEYMAPS_FILE`,
  selects compiled firmware defaults; reset restores those defaults.

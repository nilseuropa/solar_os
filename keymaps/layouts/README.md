# Keyboard language layouts

A language layout turns a USB HID keyboard usage into a character, for
`setterm keyboard <name>`. The physical side (which key sends which usage) is
separate and lives in `keymaps/profiles/`.

The JSON files here are generated from
[xkeyboard-config](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config),
the layout database used by X11 and Wayland, so every language comes from one
maintained source. Do not edit them by hand; fix the generator or XKB instead.

## Files

- `keymaps/layouts/<name>.json`: one layout.
- `scripts/generate_keyboard_layouts.py`: host tool, XKB to JSON.
- `scripts/compile_keyboard_layouts.py`: runs in the firmware and host-test
  builds and turns the built-in layouts into C tables
  (`solar_os_keyboard_layouts_generated.h`).
- `src/services/solar_os_keyboard_layout.h`: the table format.

## Schema

```json
{
  "name": "de",
  "description": "German",
  "source": {"xkeyboard-config": "2.48", "layout": "de", "variant": ""},
  "keys": {
    "0x04": {"levels": ["a", "A", "æ", "Æ"], "caps": ["A", "a", "Æ", "æ"]},
    "0x35": {"levels": [{"dead": "circumflex"}, "°", "′", "″"]}
  }
}
```

- `keys` maps a HID usage (`0x` and two lowercase hex digits) to what the key
  types. A layout may only define usages whose meaning depends on the language:
  letters, digits, punctuation, Space, the ISO key left of Z (`0x64`) and the JIS keys
  `0x87`/`0x89`. Enter, Escape, Backspace, Tab, function, navigation and keypad
  keys are the same in every layout and stay in C.
- `levels` is what the key types with no modifier, Shift, AltGr (right Alt)
  and Shift+AltGr, in that order. `caps` is the same four values while Caps
  Lock is on; it is omitted when Caps Lock changes nothing. Missing trailing
  values and `null` mean the key types nothing.
- A value is one Unicode character from the Basic Multilingual Plane, or
  `{"dead": "<name>"}` for an XKB dead key (`dead_circumflex` and so on).
- The values are what XKB resolves for that key and modifier state, so key
  types, AltGr fallbacks and Caps Lock rules are already applied.

## Regenerating and adding layouts

Install libxkbcommon and xkeyboard-config (Debian/Ubuntu:
`libxkbcommon0 xkb-data`; Homebrew: `libxkbcommon xkeyboard-config`), then:

```sh
scripts/generate_keyboard_layouts.py --all        # refresh every file
scripts/generate_keyboard_layouts.py fr           # XKB layout "fr"
scripts/generate_keyboard_layouts.py ch-fr --xkb ch --variant fr
```

`--xkb-root` selects another XKB data directory. The XKB data version is
recorded in each file. The model is always `pc105`.

A new file is not built in until its name is appended to `BUILTIN_LAYOUTS` in
`scripts/compile_keyboard_layouts.py` and to the input service's layout list.
The firmware stores the selected layout's index, so append only; never reorder
or remove built-in layouts.

## Current limits

- Key events carry one byte: ASCII, or Latin-1 letters from U+00C0. Bytes
  0x80-0xbf are SolarOS key codes. Other characters in a layout (`§`, `°`,
  `€`, Cyrillic, ...) type nothing yet; a Caps Lock result that cannot be sent
  falls back to the character without Caps Lock.
- Dead keys type their spacing character (`^`, `` ` ``, `~`, ...) instead of
  combining with the next key.
- Input methods (CJK) and right-to-left or shaped text are out of scope.

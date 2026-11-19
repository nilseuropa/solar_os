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
  "source": {"xkeyboard-config": "2.48", "libxkbcommon": "1.13.2", "libx11": "1.8.14",
             "layout": "de", "variant": ""},
  "keys": {
    "0x04": {"levels": ["a", "A", "æ", "Æ"], "caps": ["A", "a", "Æ", "æ"]},
    "0x35": {"levels": [{"dead": "circumflex"}, "°", "′", "″"]}
  },
  "dead": {
    "circumflex": {" ": "^", "A": "Â", "a": "â"}
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
- `dead` lists every dead key the layout uses, with its compositions from the
  libX11 `en_US.UTF-8` Compose table: the character that a dead key followed
  by another character types. Only two-key sequences whose second character
  the layout types are kept. The space entry is the spacing character the dead
  key types on its own.

## Regenerating and adding layouts

The output depends on three packages, not only on the XKB data: libxkbcommon
applies Caps Lock through Unicode case mappings that change between releases
(for example, 1.8.0 added ß → ẞ), and the dead key compositions come from
libX11's Compose table. The generator therefore requires the exact versions
pinned in `REQUIRED_VERSIONS` in `scripts/generate_keyboard_layouts.py`
(currently libxkbcommon 1.13.2, xkeyboard-config 2.48 and libX11 1.8.14),
records them in each file, and refuses to run with others. The firmware build
rejects a file that records other versions.

The generator finds them through their pkg-config files (`xkbcommon.pc`,
`xkeyboard-config.pc`, `x11.pc`; no `pkg-config` binary needed) in the usual
system and Homebrew directories or `PKG_CONFIG_PATH`, loads libxkbcommon from
its `libdir` and reads `share/X11/locale/en_US.UTF-8/Compose` from the libX11
prefix. Homebrew `libxkbcommon xkeyboard-config libx11` currently matches the
pins; distribution packages are often older, in which case build them from
source into a prefix and point `PKG_CONFIG_PATH` at it. Then:

```sh
scripts/generate_keyboard_layouts.py --all        # refresh every file
scripts/generate_keyboard_layouts.py fr           # XKB layout "fr"
scripts/generate_keyboard_layouts.py ch-fr --xkb ch --variant fr
```

The model is always `pc105`. To move to newer versions, change the pins and
regenerate every layout in the same commit. `tests/test_keyboard_layouts.py`
checks that the committed files match a fresh generation when the pinned
versions are installed, and says why it is skipped otherwise.

A new file is not built in until its name is appended to `BUILTIN_LAYOUTS` in
`scripts/compile_keyboard_layouts.py` and to the input service's layout list.
The firmware stores the selected layout's index, so append only; never reorder
or remove built-in layouts.

## Built-in layouts

`us`, `de`, `ua` (Ukrainian), `fr` (French AZERTY) and `ro` (Romanian).

`setterm keyboard us,ua` saves an ordered list. Ctrl+Space cycles through it
once per press without writing NVS, and the status bar shows the active layout
as `US`, `UA`, etc. Boot starts with the first entry. A single layout disables
the shortcut; exclusive raw keyboard captures retain their Ctrl+Space.

## How keys type

- Apps with `SOLAR_OS_APP_FLAG_TEXT_EVENTS` (Writer) get every layout
  character as a UTF-8 `SOLAR_OS_EVENT_TEXT`, Cyrillic included. A dead key
  waits for the next character and types the composition (`^` then `e` is
  `ê`); dead key + space or the dead key twice types its spacing character,
  and a character without a composition types the spacing character followed
  by that character. Enter, Backspace, navigation, Ctrl keys and a change of
  input focus drop a pending dead key.
- Other apps get key bytes as before: ASCII, or Latin-1 letters from U+00C0
  (bytes 0x80-0xbf are SolarOS key codes). Characters outside that range
  (`§`, `€`, Cyrillic, ...) type nothing for them, a Caps Lock result that
  does not fit falls back to the character without Caps Lock, and a dead key
  types its spacing character.
- Internally such a key arrives as `SOLAR_OS_KEY_LAYOUT_TEXT` or
  `SOLAR_OS_KEY_LAYOUT_TEXT_CAPS` and is resolved from the event's usage and
  modifiers (`solar_os_input_resolve_layout_key()`), so keyboard drivers keep
  passing one byte. Input consumers (apps, captured keys) see the resolved
  byte, never these codes.
- Shortcuts: Ctrl+letter uses the letter on the key (Ctrl+A on the AZERTY A
  key). Keys whose letter is not Latin (Cyrillic) use their US character with
  Ctrl, left Alt and GUI.

## Current limits

- The built-in fonts cover Latin-1, so characters outside it (Cyrillic, `ș`,
  `€`, ...) are stored as typed but not drawn yet.
- Dead key sequences are two keys; longer Compose sequences (`^` `´` `a`) and
  a Compose key are not supported. Nothing on screen shows a pending dead key.
- Input methods (CJK) and right-to-left or shaped text are out of scope.

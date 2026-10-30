+++
id = "navigation"
title = "Navigation and keys"
section = "concept"
summary = "Learn common shortcuts and built-in controls on compact targets"
aliases = ["shortcuts"]
keywords = "navigation keys keyboard shortcuts controls combinations escape enter alt ctrl shift pager t-lora-pager t-deck cl-32 odroid wheel trackball"
packages_any = []
+++
# Navigation and keys

SolarOS accepts input from keyboards, board buttons, wheels, and trackballs.
Use the common keys below, or find your device's built-in controls in the
target tables. These tables describe the default mappings; custom keyboard
profiles can change them.

`Alt+Enter` means hold Alt, press Enter, then release both keys. A **tap**
means press and release a key. A **hold** keeps a modifier or layer active
while you press another key.

## Basic navigation

| Key | Action |
| --- | --- |
| Up / Down | Move between menu items or scroll, depending on the app. |
| Left / Right | Move across the launcher grid; move the cursor or change a value where supported. |
| Enter | Select an item, open it, or confirm a prompt. |
| Escape | Go back or cancel; exits apps that support Escape. |
| Page Up / Page Down | Move a page at a time where supported. |
| Home / End | Move to the start or end where supported. |
| Alt+Enter | Toggle full screen in shared text menus. Small menus start in full screen automatically. |

Run `launcher` for the graphical app grid, or type an app's command name at
the shell prompt. App-specific controls appear in the app's help and the
[Application reference](apps.md). Escape is handled by each app; use the
app-close shortcut below when you need to close the foreground app.

## Sessions and app shortcuts

| Action | Local display keyboard | Port terminal (USB CDC, UART, Telnet) |
| --- | --- | --- |
| Close the foreground app | Ctrl+Alt+Del | Ctrl+] |
| Next retained session | Alt+Tab or Alt+Right | Ctrl+Z suspends a resumable app and returns to the prompt; use `fg` to resume it. |
| Previous retained session | Alt+Left | Use `fg ID` to resume a specific retained app. |
| Move local keyboard focus to another display | Ctrl+Alt+Arrow | Run `session focus TARGET`. |

Either Alt key works for session switching and display focus, including
AltGr on compact keyboards. Switching sessions retains resumable apps;
closing an app returns to its display's shell or resumes its parent app.
Use `session list` to see session IDs and [Sessions](sessions.apps.md) for
more controls.

## At the shell prompt

| Key | Action |
| --- | --- |
| Enter | Run the typed command. |
| Up / Down | Browse command history. |
| Left / Right | Move the insertion cursor. |
| Backspace / Delete | Delete before / at the cursor. |
| Escape | Clear the typed command without running it. |
| Tab | Complete a command, subcommand, or path. |
| Tab twice | List matching completions; at an empty prompt, list commands. |

## LilyGO T-LoRa-Pager (T-Pager)

| Control | Action |
| --- | --- |
| Wheel clockwise | Down — next menu item or scroll down. |
| Wheel counterclockwise | Up — previous item or scroll up. |
| Press wheel | Enter — select/open; emitted when the wheel is released. |
| Keyboard Enter | Enter. |
| Hold Space + Enter | Escape — back/cancel; exits apps that support Escape. |
| Tap Space | Type a space. |
| Hold Space + another key | Type its symbol; Q–P give digits 1–0. |
| Hold the Caps-labelled key | Shift — use with another key for its shifted character. |
| Hold Alt | Alt modifier. |
| Backspace | Delete the character before the cursor. |

The default keyboard profile has no Left, Right, Tab, or Ctrl mappings.
The wheel supplies Up/Down, so it cannot move across launcher columns.
Use a custom keyboard profile or a compatible external keyboard for those
keys, horizontal launcher navigation, and the usual session-switch and
app-close combinations. You can also launch apps by typing their command
names. Space+Enter remains Escape rather than the universal app-close key.

## LilyGO T-Deck Plus

| Control | Action |
| --- | --- |
| Roll trackball up / down / left / right | One arrow key in the dominant direction after movement stops. |
| Tap trackball | Enter — select/open by default. |
| Hold trackball | Suspend by default. |
| Keyboard Enter | Enter. |
| Tap microphone-icon key (Mic) | Tab on release — complete a shell command or argument; tap twice to list matches. |
| Shift + microphone-icon key | Ctrl — keep Mic held while pressing the other key. |
| Sym + microphone-icon key | Type `0`. |
| Shift + trackball arrows | Extend the text selection in the editor. |
| Shift + Mic + trackball left / right | Move the cursor one word left / right. |
| Alt+C | Escape — back/cancel where supported. |
| Alt+Q | Close the foreground app. |
| Alt+B | Toggle the keyboard backlight for the current boot; the shortcut does not type a character. |
| Alt + trackball up / down | Page Up / Page Down. |
| Alt + trackball left / right | Previous / next retained session on the focused display. |
| Alt + Mic | Next retained session on the focused display. |
| Alt+Enter | Toggle full screen in shared text menus. |
| Hold Sym + another key | Type the symbol from the keyboard's symbol layer. |
| Hold Shift + another key | Type its shifted character. |

The trackball press uses the system KEY settings. Change a tap with
`setterm powerkey short <action>` and a hold with
`setterm powerkey long <action>`, or edit the two rows in `setterm`.
Saved choices override these defaults. Any KEY press wakes suspend or sleep;
the wake press is consumed until release. Deep-sleep wake starts a fresh boot.

Trackball movement is evaluated in fixed 80 ms windows. The first evaluation
presses an arrow; continued rolling keeps it held, using `setterm keyrate` for
repeat. Changing direction releases the old arrow before pressing the new one.
The key is released after 80 ms without movement events. Short rolls that meet
the minimum vector length produce a press and release.
The direction follows the greater net horizontal or vertical movement within
each window; opposite movements cancel. Equal diagonals use the most recently
moved axis. Each evaluation uses the modifiers from its last movement pulse.

The board attaches the `trackball` expansion driver as `trackball0` and starts
the `pointer-arrows` job. The driver owns the four direction pins and publishes
relative pointer movement. The job consumes that source and converts movement
to the arrow gestures described above. To use relative pointer input directly:

```sh
job stop pointer-arrows
```

Restore arrow navigation with:

```sh
job start pointer-arrows source=trackball0
```

To reduce sensitivity, stop the job and restart it with, for example,
`job start pointer-arrows source=trackball0 min-vector=4`. `min-vector` accepts
`1..65535` and defaults to `2`. Higher values require a longer net movement
vector within each 80 ms window. Movement below the minimum releases the arrow
and does not accumulate into the next window.

The push switch continues using the system KEY settings in either case. The
board starts arrow navigation again at boot; a startup script can stop the job
to select relative pointer input, or stop and restart it with a tuned
`min-vector`, on every boot.

For Ctrl, hold Shift and Mic in either order, then press another key or roll
the trackball. Shift selects the Ctrl chord and is consumed, so this does not
also select text. Shift can be released while Mic continues acting as Ctrl;
releasing Mic releases Ctrl. A plain Mic emits Tab on release, allowing the
Ctrl chord to be recognized without triggering completion. Sym+Mic retains
its digit `0` mapping.

Keyboard keys report their actual presses and releases, supporting held controls
in foreground apps. Trackball arrows also report presses and releases;
`setterm keyrate` controls repeat for typing and held trackball arrows.

The keyboard light starts disabled on every boot, retaining its saved brightness.
Alt+B toggles the light for the current boot. Suspend and light sleep turn it
off temporarily, then restore the user's on/off choice on wake. An explicit
`setterm backlight` level or `follow` applies that setting for the current boot.
Use `setterm backlight 0` to turn off the keyboard light, or
`setterm backlight 100` for full brightness. Values from 0 to 100 set it
independently of display brightness; `setterm backlight follow` makes it track
the display. Alt+B does not save changes across reboots. When the configured
brightness is zero, toggling on uses 50%. The `setterm backlight` query shows
the configured brightness, including while the light is toggled off.

## Pi0cket CL-32

| Control | Action |
| --- | --- |
| Arrow keys | Corresponding arrow key. |
| OK | Enter — select/open. |
| Cancel | Escape — back/cancel where supported. |
| Hold File | Left Alt modifier. |
| Hold Menu | Left Ctrl modifier. |
| File + Left / Right | Previous / next retained session on the focused display. |
| File+OK | Toggle full screen in shared text menus. |
| Hold Shift | Shift modifier. |
| Hold Fn | Select the alternate symbol and function-key layer. |
| Fn + keypad 1–9 / 0 | F1–F9 / F10. |
| Fn + Up / Down | Page Up / Page Down. |
| Fn + Left / Right | Home / End. |
| Fn + Yes (OK) / No (Cancel) | Previous / next retained session on the focused display, once per key press. |

Shift and Fn stay active only while held. Release them to return to normal
typing.

The built-in keyboard resumes `suspend` and wakes `sleep` or `deepsleep`.
Wake from `deepsleep` starts a fresh boot. Keyboard, USB-power, and charging
notifications share the wake interrupt, so power changes can also wake sleep.

## ODROID-GO

| Control | Action |
| --- | --- |
| D-pad | Corresponding arrow key, when ADC D-pad support is included in the firmware. |
| A | Enter — select/open. |
| B | Space. |
| SELECT | Escape — back/cancel where supported. |
| MENU | Close the foreground app. |
| VOLUME | Toggle audio mute. |

The built-in buttons do not provide a text keyboard or Alt/Ctrl modifiers.
Use a compatible external keyboard or a port terminal for typing commands
and combinations that need those keys.

See [Boards](boards.md) for target capabilities and
[Input](media.input.md#keyboard-input) for input sources and keyboard layout
settings. To customize a supported keyboard profile, use the website's
Tools → Keyboard configurator.

## Quick reference

Arrows navigate, Enter selects, and Escape goes back where supported.
Close a local display app with Ctrl+Alt+Del; in a port terminal use Ctrl+].
Alt+Tab or Alt+Right selects the next local session; Alt+Left selects the
previous one. Ctrl+Z suspends a resumable port app; fg resumes it.
T-Pager: wheel is Up/Down, press is Enter, Space+Enter is Escape, held Space
selects symbols, and held Caps is Shift. Its default profile lacks Left,
Right, Tab, and Ctrl. T-Deck Plus: trackball supplies arrows and Enter,
Mic is Tab, Shift+Mic is Ctrl, Alt+C is Escape, and Alt+Q closes the app.
Pi0cket CL-32: OK is Enter, Cancel is
Escape, File is Alt, and Menu is Ctrl. ODROID-GO: D-pad supplies arrows,
A is Enter, B is Space, SELECT is Escape, and MENU closes the app.

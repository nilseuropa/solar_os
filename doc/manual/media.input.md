+++
id = "media.input"
title = "Audio, input, and clipboard APIs"
section = "hardware"
summary = "Use installed media and generic input services"
aliases = ["audio", "ble", "clipboard", "pointer", "mouse", "touch", "joystick"]
keywords = "python lua audio speaker microphone wav tone ble bluetooth keyboard pointer mouse touch joystick gesture calibration clipboard"
packages_any = []
+++
# Audio, input, and clipboard APIs

These services are independent even though they are often used by foreground
applications. Inspect availability before calling an optional audio or BLE
operation.

## Keyboard input

BLE, PS/2, CardKB, integrated keyboards, board buttons, `gpio-keys`, and ADC
D-pads feed the same input service. Use `setterm keyboard us` or `de` to choose
a text layout, and `setterm keyrate` to set key repeat. Analog joysticks report
axes; they do not generate arrow keys.

On a board with absolute touch and PSRAM, `job start graffiti` turns the whole
display into a Palm Graffiti handwriting keyboard without drawing an overlay.
The stroke's initial X coordinate selects its alphabet: the left two-thirds is
letters and the right one-third is numbers. The selection does not change
while the stroke is in progress. Recognized strokes enter the normal keyboard
queue, so they follow the current input focus. The job observes absolute touch
without consuming it; foreground applications still receive the pointer
events. See
[jobs.reference.md](jobs.reference.md#graffiti) for controls and ownership.

`input test <source>` counters are cumulative from the time that source
attached. Each accepted key press, release, or repeat increments `key`; it is
not a count of currently held keys. Character-only devices such as CardKB emit
one press and one release per character, so one tap normally adds two events.
Their last event has `physical=0` and `usage=0`; `key=10` is newline/Enter.

On Pi0cket CL-32, hold Shift for shifted keys and Fn for alternate symbols.
Both modifiers stop applying when released. File is left Alt, Menu is left Ctrl,
OK is Enter, Cancel is Escape, and holding Fn with keypad 1 through 0 produces
F1 through F10. Hold Fn and press Yes (OK) or No (Cancel) to switch to the
previous or next retained session on the focused display, once per key press.

## PS/2 keyboard

PS/2 uses a named, exclusive CLOCK/DATA bus. Attach a keyboard device to that
bus:

```text
expansion bus create ps2 ps2kbd clock=gpio17 data=gpio18
expansion attach ps2-keyboard keyboard0 ps2=ps2kbd
input keyboard
input test keyboard0
```

This manual setup is for an attached expansion keyboard. A board with an
integrated PS/2 keyboard capability, such as TTGO VGA32 v1.4, declares its bus
and default expansion attachment automatically during boot. The compatible
`ps2-keyboard` job remains as a wrapper for existing scripts.

The receiver supports keyboard scan-code set 2, including normal and extended
press/release sequences. SolarOS supplies repeat through the generic input
service, so keyboard typematic make codes do not create duplicate presses.
The keyboard attachment is receive-only: keyboard LEDs and keyboard-specific
host commands are not implemented.

ESP32 pins are not 5 V tolerant. Use proper level shifting, or otherwise ensure
that neither PS/2 signal can be pulled above 3.3 V. Do not connect a 5 V signal
directly to a GPIO merely because PS/2 uses open-collector signalling.

## Pointers and axes

Use `input` to inspect semantic sources independently of their transport:

```text
input touch
input mouse
input joystick
input gesture
input test touch0
```

Touch and other absolute pointers report positions; mice report relative
deltas; analog joysticks report normalized X/Y axes; gesture sensors report
recognized motions and taps. `input test` shows counters and the latest event
while the shell is active.

The input service recognizes single-finger `tap`, `double-tap`, `hold`, and
`swipe` gestures from any registered absolute touch source. Producers submit
ordinary pointer samples; the common input pump handles gesture timing.
`gesture status` lists touch sources and their supported gestures. Recognition
preserves ordinary pointer events and pauses while an exclusive pointer filter
or capture owns the source, including handwriting input.

A tap lasts at most 250 ms with no more than 12 pixels of movement on either
axis. A single tap waits 300 ms after release so a second tap can form one
`double-tap` event instead of two taps. The second press must start within that
interval and within 24 pixels on each axis of the first release. A stationary
contact produces one `hold` after 600 ms; release does not produce another
gesture. A swipe completes within 1000 ms, moves at least 40 pixels, and has a
dominant axis at least 1.5 times the other axis. Its direction is `west`,
`east`, `north`, or `south` in calibrated source coordinates, before display
`setterm orientation` is applied. Other touch gestures use direction `none`.
Touch gesture `value` is 1 for tap, 2 for double-tap, 600 milliseconds for hold,
or the dominant-axis distance in pixels for swipe, capped at 32767. `raw` is
zero; double-tap sets the double flag. Pinch and rotation are not supported.

Native foreground applications opt in to structured pointer input with
`SOLAR_OS_APP_FLAG_POINTER_EVENTS` and to axis input with
`SOLAR_OS_APP_FLAG_AXIS_EVENTS`. Gesture-aware applications opt in with
`SOLAR_OS_APP_FLAG_GESTURE_EVENTS`. Their event callback then receives
`SOLAR_OS_EVENT_POINTER` in `event.data.pointer` or `SOLAR_OS_EVENT_AXIS` in
`event.data.axis`, or `SOLAR_OS_EVENT_GESTURE` in `event.data.gesture`.
Pointer events contain the source, pointer ID, absolute or
relative mode, action, coordinates, deltas, buttons, and optional display
target. A non-empty target routes to the active opted-in application on that
display. Its absolute coordinates and deltas follow the target's current
`setterm orientation`; orientation `0` is the device driver's normal mounting.
An empty target follows local input focus. Axis events contain the source,
X/Y/Z/RX/RY/RZ axis, normalized value, and delta and follow local input focus.
Gesture events contain the recognized kind, direction, flags, optional value,
and original sensor word and also follow local input focus.
Applications without the matching flag do not receive those structured events.

Gesture bindings observe the same events without taking them away from the
foreground application. A binding can match one source or every source, one
gesture kind, and optionally one direction. Commands run one at a time. The
cooldown suppresses repeated reports and defaults to 250 ms.

```text
gesture bind source=gesture0 gesture=flick direction=east -- input emit ALT+RIGHT
gesture bind source=gesture0 gesture=flick direction=west cooldown=400 -- input emit ALT+LEFT
gesture bind source=* gesture=double-tap -- /flash/bin/toggle-light.sh
job start gestures
gesture bindings
job stop gestures
gesture unbind 2
```

To use horizontal swipes for previous/next foreground sessions and vertical
swipes for page navigation in the focused app:

```text
gesture bind source=* gesture=swipe direction=west -- input emit ALT+LEFT
gesture bind source=* gesture=swipe direction=east -- input emit ALT+RIGHT
gesture bind source=* gesture=swipe direction=north -- input emit PAGE_UP
gesture bind source=* gesture=swipe direction=south -- input emit PAGE_DOWN
job start gestures
```

`input emit` creates a virtual local keyboard on first use and injects a key
tap into the normal input-focus path. Modifier chords use `CTRL`, `SHIFT`,
`ALT`, `GUI`, or their left/right forms, for example `ALT+RIGHT`. It does not
send USB or BLE HID reports. A chord tap releases its modifiers together with
the named key.
Bindings are deliberately volatile and remain configured when the
`gestures` job stops. Put the required `gesture bind` commands followed
by `job start gestures` in the selected startup shell script to recreate
and activate them after boot. `gesture unbind all` removes every rule and resets
the next binding ID to 1. Background commands may use shell built-ins or invoke
scripts, but they cannot launch a foreground application. Stopping the job
prevents new gesture actions and discards queued actions; a command already
executing is allowed to finish. Use an explicit display target for display
actions, for example `setterm --display display0 orientation 90`.

Foreground Python and Lua scripts receive the same structured pointer, axis, and gesture
events through `solaros.input.read([timeout_ms])`. Touch events expose absolute
`x`/`y` coordinates and press/move/release actions; relative mice expose
`delta_x`/`delta_y` and button bits; joystick events expose their named axis,
normalized value, and delta; gesture events expose names, direction, flags,
value, and raw sensor data. `solaros.input.sources()` lists the registered
semantic sources. Each runtime keeps a bounded 16-event foreground queue and
reports overwritten events through `solaros.input.status().dropped`. Keyboard
characters remain on `solaros.tui.getch()`.

Absolute-pointer calibration maps a source's raw logical coordinates into a
target extent and stores the mapping in NVS under that source name:

```text
input calibrate touch0
input calibrate touch0 set 0 479 0 319 480 320
input calibrate touch0 reset
```

The device driver still owns physical orientation. Calibration only clamps and
scales the already oriented X/Y values. Mouse deltas and joystick axes do not
use pointer calibration.

Attach a standard relative PS/2 mouse to a named bus with:

```text
expansion bus create ps2 ps2mouse clock=gpio17 data=gpio18
expansion attach ps2-mouse mouse0 ps2=ps2mouse
input test mouse0
```

The mouse attachment enables standard three-byte reporting and publishes
relative motion plus the primary, secondary, and middle buttons. Confirm the
connector's pinout, supply, and signal voltage before wiring it; ESP32 GPIOs
are not 5 V tolerant.

An analog joystick consumes two existing scalar streams. Use `stream list` and
`adc status` to find the actual stream names, then bind the measured range:

```text
expansion attach analog-joystick joystick0 x=adc2 y=adc4 min=0 center=1650 max=3300 deadzone=100
input test joystick0
```

The attachment normalizes both streams as axes. Applications decide what those
axes mean; SolarOS does not turn them into arrows or other keys.

## Audio

Use global volume unless a diagnostic or playback command explicitly needs an
override. Call `deinit()` or `off()` when a script owns output that should not
remain active.

Global volume follows the selected playback device, including runtime-attached
outputs on boards without built-in audio. Selecting or opening a volume-capable
output applies the current global value, and Player, WebRadio, Recorder, and
Synth initialize their volume controls from that shared state.

Recording and playback require enough internal/DMA memory even on boards with
PSRAM. If an audio application reports no memory, stop unnecessary internal
stack jobs and inspect `mem`.

`tone_async()` queues a short tone and returns a request ID without waiting for
playback. Use `cancel()` with that ID or inspect `queue_status()` for the
current request and completed, cancelled, dropped, and failed counters. The
queue is bounded and shares exclusive output ownership with WAV playback and
native synth clients. A queued tone waits for that output; a full queue reports
an error to the caller.

`solaros.synth` gives Python and Lua scripts an eight-voice native synthesizer
with two oscillators per voice, square, triangle, saw, sine, and noise
waveforms, velocity, amplifier and filter ADSR envelopes, and resonant low-pass
filters. The second oscillator adds octave, fine-detune, and mix controls. The
interpreters send note and configuration commands; rendering stays in the
native audio task. The first note claims exclusive audio output, and script
shutdown releases it automatically. Synth voices use global speaker volume
rather than overriding it.

## BLE keyboard

The BLE service manages one remembered keyboard and publishes its HID report
transitions through the generic input service. Pairing and scanning are system
operations; a script can inspect state and read translated key events.
BLE follows the board default when no user preference is saved. Most boards
enable it; TTGO VGA32 v1.4 disables it to preserve internal heap. Use `setterm
ble on|off|default` to select an explicit next-boot value or return to the board
default. The compatible `ble enable`, `ble disable`, and `ble default` commands
provide the same settings. These commands leave the current boot unchanged and
do not forget the remembered keyboard or its bond. Disabling BLE also frees internal memory for other work on the next boot.
`ble forget` erases the remembered keyboard from SolarOS NVS and removes its BLE
bond and cached GATT service database. This forces service rediscovery when the
same keyboard address is paired again, including after keyboard firmware changes
move its GATT handles. `ble pair` performs that forget operation before it scans
for a replacement, so switching keyboards does not retain the previous keyboard
bond. If a keyboard still has an orphaned bond from an older SolarOS release,
explicit pairing replaces that conflicting bond. On boards with a system KEY,
`setterm powerkey long ble-pair` assigns this operation to a hold. It is the
default on targets without a board-owned wired
keyboard; targets with one default to suspend. Pairing has no user cancellation
path. The short and long actions are independently configurable with
`setterm powerkey short <action>` and `setterm powerkey long <action>`.
A press of any duration resumes suspend, with the wake press consumed until
release. See the [KEY settings](commands.md) for actions and board defaults.

## Clipboard

The clipboard stores bounded text shared by applications. Clear sensitive
content after use.

## Quick reference

solaros.audio provides status, deinit or off, set_volume, set_mic_gain, tone,
tone_async, cancel, queue_status, level, capture, loopback, wav_info,
record_wav, and play_wav. `capture(frames)` returns 1 through 4096 frames as
interleaved little-endian signed-16 PCM plus its native sample format, rate,
channel count, and sample width. solaros.synth provides status, configure, configure_oscillator2,
configure_filter, configure_performance, note_on, note_off, all_notes_off, and
stop. solaros.ble provides status, connected, pair, forget, layout, read.
solaros.clipboard provides set, get, size, clear. Audio, synth, and BLE are
package-gated. Foreground Python and Lua applications use solaros.input sources,
read, clear, and status for structured pointer, axis, and gesture events; keyboard
characters remain on solaros.tui.getch().

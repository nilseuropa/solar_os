+++
id = "media.input"
title = "Audio, input, and clipboard APIs"
section = "hardware"
summary = "Use installed media and generic input services"
aliases = ["audio", "ble", "clipboard", "pointer", "mouse", "touch", "joystick"]
keywords = "python lua audio speaker microphone wav tone ble bluetooth keyboard pointer mouse touch joystick calibration clipboard"
packages_any = []
+++
# Audio, input, and clipboard APIs

These services are independent even though they are often used by foreground
applications. Inspect availability before calling an optional audio or BLE
operation.

## Keyboard input

Local hardware input uses structured press, release, and repeat events. The
input service tracks held keys by source and stable physical identity, while
legacy shell and text applications continue to receive translated characters.
BLE, PS/2, and CardKB keyboards, fixed board buttons, `gpio-keys`, and ADC
D-pads share one repeat policy. Configure it with `setterm keyrate`; the
setting applies even on a build without BLE. Analog joysticks are different:
they publish normalized axes and never synthesize key events.

Keyboard transports can additionally supply a canonical USB HID usage and
modifier mask. This keeps physical controls independent of the selected text
layout and lets BLE and PS/2 share the same US or German keymap.

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
input test touch0
```

Touch and other absolute pointers report positions; mice report relative
deltas; analog joysticks report normalized X/Y axes. Pointer and axis queues
are allocated only when the first matching source attaches. `input test`
retains counters and the most recent accepted event, so it also works for
polling touch controllers while the shell is active.

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
do not forget the remembered keyboard or its bond. On a BLE-disabled boot, the
unused Bluetooth controller and host memory is returned to the internal heap
before normal service initialization.
`ble forget` erases the remembered keyboard from SolarOS NVS and removes its BLE
bond. On boards with a system KEY, a long press performs that forget operation
and then starts a new pairing scan. Pairing has no user cancellation path. The
KEY short-press power action remains separately configurable with
`setterm powerkey sleep|suspend`. Suspend is the default; another short press
resumes the display and restores the prior power profile.

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
package-gated.

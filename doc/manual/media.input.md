+++
id = "media.input"
title = "Audio, keyboard input, and clipboard APIs"
section = "hardware"
summary = "Use installed media and input services"
aliases = ["audio", "ble", "clipboard"]
keywords = "python lua audio speaker microphone wav tone ble bluetooth keyboard clipboard"
packages_any = []
+++
# Audio, keyboard input, and clipboard APIs

These services are independent even though they are often used by foreground
applications. Inspect availability before calling an optional audio or BLE
operation.

## Keyboard input

Local hardware input uses structured press, release, and repeat events. The
input service tracks held keys by source and stable physical identity, while
legacy shell and text applications continue to receive translated characters.
BLE and PS/2 keyboards, fixed board buttons, `gpio-keys`, joysticks, and ADC D-pads share
one repeat policy. Configure it with `setterm keyrate`; the setting applies even
on a build without BLE.

Keyboard transports can additionally supply a canonical USB HID usage and
modifier mask. This keeps physical controls independent of the selected text
layout and lets BLE and PS/2 share the same US or German keymap.

## PS/2 keyboard

PS/2 uses a named, exclusive CLOCK/DATA bus and a background input job:

```text
expansion bus create ps2 ps2kbd clock=gpio17 data=gpio18
job start ps2-keyboard ps2kbd
```

The receiver supports keyboard scan-code set 2, including normal and extended
press/release sequences. SolarOS supplies repeat through the generic input
service, so keyboard typematic make codes do not create duplicate presses.
This first implementation is receive-only: keyboard LEDs and host-to-keyboard
commands are not implemented.

ESP32 pins are not 5 V tolerant. Use proper level shifting, or otherwise ensure
that neither PS/2 signal can be pulled above 3.3 V. Do not connect a 5 V signal
directly to a GPIO merely because PS/2 uses open-collector signalling.

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
BLE is enabled by default. `ble disable` prevents the BLE stack from starting
on the next boot, and `ble enable` enables it again for the next boot. Both
commands leave the current boot unchanged, and neither command forgets the
remembered keyboard or its bond. On a BLE-disabled boot, the unused Bluetooth
controller and host memory is returned to the internal heap before normal
service initialization.
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

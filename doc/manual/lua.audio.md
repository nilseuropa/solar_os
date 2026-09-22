+++
id = "lua.audio"
title = "Lua audio and control API"
section = "api"
summary = "Audio and control: audio, synth, dsp, controls, parameters, midi, osc"
keywords = "lua solaros api audio audio synth dsp controls parameters midi osc"
packages_any = ["app_lua"]
agent_reference_sections = true
+++
# Lua audio and control API

[API overview](lua.md) · [Python audio and control](python.audio.md)

## `solaros.controls`

- `solaros.controls`: `list`, `get`, `set`, `create`, `delete`, `clear`, `bindings`, `bind_parameter`, `bind_midi`, `unbind` when continuous controls are compiled. Values use the normalized range `0.0..1.0`.

## `solaros.parameters`

- `solaros.parameters`: `list`, `get`, `set` for dynamic native application parameters when continuous controls are compiled.

## `solaros.midi`

- `solaros.midi`: `status`, `send`, `note_on`, `note_off`, `cc`, `program`, `read`/`receive`, `close`, `streams`, `stream_add`, `stream_remove`, `stream_clear` when MIDI support is compiled.

## `solaros.osc`

- `solaros.osc`: `bindings`, `bind_stream`, `bind_event`, `bind_control`, `unbind`, `clear`, `encode_float`, `encode_int`, `dispatch`, `limits` when OSC support is compiled.

## `solaros.dsp`

- `solaros.dsp`: `backend`, `capabilities`, `dot`, `gain`, `mix`, `clip`, `level`, `window`, `fir`, and `fft` when `service.dsp` is compiled. Binary strings contain native little-endian signed 16-bit values; FIR and FFT constructors return caller-owned userdata.

## `solaros.audio`

- `solaros.audio`: `status`, `deinit`, `off`, `set_volume`, `set_mic_gain`, `tone`, `tone_async`, `cancel`, `queue_status`, `level`, `capture`, `loopback`, `wav_info`, `record_wav`, `play_wav` when audio support is compiled. `capture(frames)` accepts 1 through 4096 frames and returns an interleaved little-endian signed-16 binary string plus a format table with `sample_format`, `sample_rate`, `channels`, and `bits_per_sample`.

## `solaros.speech`

- `solaros.speech`: `say`, `cancel`, `request_status`, and `queue_status` when
  offline speech is compiled. Start `speechd` first. `say(text[, volume[,
  drop_if_busy[, pitch[, speed]]]])` copies up to 512 UTF-8 bytes, returns
  immediately, and returns a request ID. Pitch accepts 50 through 200 and
  speed accepts 20 through 500; both default to 100.

```lua
solaros.jobs.start("speechd", {"/voices/en-US"})
local utterance = solaros.speech.say("Solar O S is ready")
print(solaros.speech.request_status(utterance).state)
```

The selected directory must contain `ta.bin` and `sg.bin`; restarting the job
with another voice directory changes the language. Accepted requests survive
interpreter exit. Stopping `speechd` cancels the queue and releases the PicoTTS
engine and voice buffers.

## `solaros.synth`

- `solaros.synth`: `status`, `configure`, `configure_oscillator2`, `configure_filter`, `configure_performance`, `note_on`, `note_off`, `all_notes_off`, `stop` when synth support is compiled. It provides eight native two-oscillator voices with polyphonic or monophonic last-note playback, portamento, per-note velocity, ADSR envelopes, and resonant low-pass filters; scripts retain the system's global speaker volume. Status includes DSP-derived `pcm_peak` and `pcm_rms` values for the captured scope block.

## MIDI

The MIDI job must own a running bus before Lua transmits or receives. The
following setup creates a bus, starts the worker, sends a note, and waits up to
one second for a non-consuming subscriber message:

```lua
solaros.buses.create_midi("midi0", { tx = 2, rx = 3 })
solaros.jobs.start("midi", { "midi0" })
solaros.midi.note_on(1, 60, 100)
local message = solaros.midi.read(1000)
```

`status()` includes traffic, parser, drop, error, CC-stream, and script
subscription state. `send(status[, data1, data2])` validates raw messages;
`note_on`, `note_off`, `cc`, and `program` provide channel-oriented helpers.
`read()` and its `receive()` alias return a message table or `nil`, are bounded
to 60 seconds, and are cancellation-aware. The subscription is automatically
released when Lua exits; `close()` releases it earlier.

Use `streams()`, `stream_add(channel, controller)`, `stream_remove(...)`, and
`stream_clear()` to manage incoming CC scalar streams. Message tables contain
`status`, `length`, `type`, and applicable channel/data fields.

## Open Sound Control

Lua configures native OSC bindings while the `osc` job retains UDP socket,
filtering, and rate-limit ownership:

```lua
solaros.osc.bind_stream(
    "ambient", "temperature", "/room/temperature", 2.0, 0.1
)
solaros.jobs.start(
    "osc", { "listen=9000", "target=192.168.1.50:9001" }
)
```

`bindings()` returns source configuration plus availability, values, timing,
send counters, and errors. `bind_stream`, `bind_event`, and `bind_control`
return numeric IDs; `unbind` and `clear` remove definitions. Event edges are
`"rising"`, `"falling"`, or `"both"`; rates are `0.1..100` Hz.

`encode_float()` and `encode_int()` return binary OSC messages that can be sent
with `solaros.net.udp_send()`. `dispatch(packet)` validates a message or
immediate bundle and applies the same native parameter routes as the job.
`limits()` reports all public codec and binding bounds.

## Synthesizer example

For example, this plays a short saw-wave chord without running Lua in the
real-time render callback:

```lua
solaros.synth.configure("saw", 5, 80, 65, 140)
solaros.synth.configure_oscillator2("square", 0, 7, 35)
solaros.synth.configure_filter(1200, 35, 80, 5, 250, 20, 180)
solaros.synth.configure_performance(true, 80)
solaros.synth.note_on(440, 110)
solaros.synth.note_on(554, 90)
solaros.time.sleep_ms(250)
solaros.synth.all_notes_off()
solaros.time.sleep_ms(150)
solaros.synth.stop()
```

`note_on()` accepts 20 through 8000 Hz and velocity 1 through 127. Envelope
times accept 0 through 10000 ms and sustain accepts 0 through 100 percent. The
`configure()` updates active voices immediately and also sets the defaults for
future notes. `configure_filter()` accepts cutoff from 40 through 18000 Hz,
resonance and envelope amount from 0 through 100 percent, followed by its own
attack, decay, sustain, and release values. The first note claims exclusive
audio output lazily, and the runtime releases it automatically when the script
exits or is interrupted.
`configure_oscillator2()` accepts waveform, octave from -2 through +2, fine
detune from -100 through +100 cents, and mix from 0 through 100 percent. Mix
zero bypasses oscillator 2 exactly. Both oscillators share the filter and
envelopes.
`configure_performance()` selects polyphonic or monophonic last-note playback
and accepts a glide time from 0 through 2500 ms.

## DSP values

For the Q15 numeric contract, processor lifetime, limits, and examples, see
[Digital signal processing](dsp.md). Lua DSP operations return new binary
strings because ordinary Lua strings are immutable.

## Quick reference

Use `solaros.audio`, `solaros.speech`, `solaros.synth`, `solaros.dsp`, `solaros.controls`, `solaros.parameters`, `solaros.midi`, `solaros.osc` for audio and control.
See `man lua` for runtime conventions and service availability.

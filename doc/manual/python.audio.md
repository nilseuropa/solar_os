+++
id = "python.audio"
title = "Python audio and control API"
section = "api"
summary = "Audio and control: audio, synth, dsp, controls, parameters, midi, osc"
keywords = "python solaros api audio audio synth dsp controls parameters midi osc"
packages_any = ["app_python"]
agent_reference_sections = true
+++
# Python audio and control API

[API overview](python.md) · [Lua audio and control](lua.audio.md)

## `solaros.controls`

Continuous controls are named normalized values. Python can configure them,
inspect their runtime counters, and supply manual values without knowing
whether their targets are native app parameters or MIDI CC messages.

- `list()`: return complete control configuration, normalized value, source
  value, generation, sample/update counters, read errors, and last error.
- `get(name)`: return the current normalized value from `0.0` through `1.0`.
- `set(name, value)`: set a manual control to a normalized value from `0.0`
  through `1.0`.
- `create(name[, source, input_min, input_max, smoothing_ms, deadband,
  inverted])`: create a manual or scalar-stream control and return its
  dictionary. Omit `source` or pass `None` for a manual control.
- `delete(name)` and `clear()`: remove one or all controls. `clear()` returns
  the number removed.
- `bindings()`: return parameter/MIDI targets and their pickup, application,
  and error state.
- `bind_parameter(name, path[, pickup])` and
  `bind_midi(name, channel, controller)`: add a target and return its numeric
  binding ID.
- `unbind(name)`: remove all targets for a control and return the count.

Create and bind a manual control directly:

```python
import solaros

solaros.controls.create("expression")
solaros.controls.bind_parameter(
    "expression", "synth.filter.resonance", False
)
solaros.jobs.start("controls")
solaros.controls.set("expression", 0.5)
print(solaros.controls.get("expression"))
```

## `solaros.parameters`

Native applications publish parameters only while they are active.

- `list()`: return path, owner, name, label, unit, range, step, curve, current
  value, readability, and error fields for every published parameter.
- `get(path)`: read a native-unit value.
- `set(path, value)`: set a native-unit value and return the authoritative
  value after range/step handling.

## `solaros.midi`

The MIDI job must own a running MIDI bus before scripts transmit or receive.
Create the bus with `solaros.buses.create_midi()` and start it with
`solaros.jobs.start("midi", ["midi0"])`.

- `status()`: return running state, bus name, RX/TX byte and message counts,
  parser/subscriber/queue drops, last error, CC-stream count, and whether this
  interpreter has an active receive subscription.
- `send(status[, data1, data2])`: validate and queue one raw MIDI message. The
  argument count must match the status byte.
- `note_on(channel, note[, velocity])`, `note_off(channel, note[, velocity])`,
  `cc(channel, controller, value)`, and `program(channel, program)`: queue
  channel messages. Channels are `1..16`; MIDI data is `0..127`.
- `read([timeout_ms])` or `receive([timeout_ms])`: lazily create a non-consuming
  interpreter subscription and return the next message dictionary, or `None`.
  Timeout is bounded to 60 seconds and is cancellation-aware.
- `close()`: release the receive subscription early. Interpreter shutdown also
  releases it automatically.
- `streams()`, `stream_add(channel, controller)`,
  `stream_remove(channel, controller)`, and `stream_clear()`: manage bounded
  incoming CC scalar streams.

Received and transmitted message dictionaries contain `status`, `length`,
`type`, optional `channel`, and the applicable `data1`/`data2` bytes.

```python
solaros.buses.create_midi("midi0", {"tx": 2, "rx": 3})
solaros.jobs.start("midi", ["midi0"])
solaros.midi.note_on(1, 60, 100)
message = solaros.midi.read(1000)
```

## `solaros.osc`

OSC bindings configure the native `osc` job; start and stop that worker through
`solaros.jobs`. The scripting API does not replace its bounded UDP transport,
peer filtering, or rate limiting.

- `bindings()`: return complete source configuration and runtime availability,
  value, timing, send, and error telemetry.
- `bind_stream(name, source, address[, rate_hz, delta, send_always])`: publish
  a scalar stream and return its binding ID.
- `bind_event(name, source, address[, edge, rate_hz])`: publish sampled event
  edges. `edge` is `"rising"`, `"falling"`, or `"both"`.
- `bind_control(name, control, address[, rate_hz, send_always])`: publish a
  normalized named control and return its binding ID.
- `unbind(name)` and `clear()`: remove one or all bindings. `clear()` returns
  the number removed.
- `encode_float(address, value)` and `encode_int(address, value)`: return a
  bounded OSC message as `bytes`, suitable for `solaros.net.udp_send()`.
- `dispatch(packet)`: validate a message or immediate bundle and apply its
  native parameter routes; return message, applied, unknown, and rejected
  counts.
- `limits()`: return packet, address, binding, bundle/update, and rate limits.

```python
solaros.osc.bind_stream(
    "ambient", "temperature", "/room/temperature", 2.0, 0.1
)
solaros.jobs.start(
    "osc", ["listen=9000", "target=192.168.1.50:9001"]
)
```

## `solaros.audio`

Available when the firmware includes the audio service.

Audio functions expose the microphone, speaker, and WAV service.

- `status()`: return codec/sample/pin status.
- `deinit()`: turn audio hardware off.
- `off()`: alias for `deinit()`.
- `set_volume(volume)`: set speaker volume.
- `set_mic_gain(gain_db)`: set microphone gain.
- `tone(frequency_hz, duration_ms[, volume])`: play a tone.
- `tone_async(frequency_hz, duration_ms[, volume])`: queue a tone and return its request ID.
- `cancel(request_id)`: cancel a queued or playing asynchronous tone.
- `queue_status()`: return asynchronous tone worker, queue, and result counters.
- `level(duration_ms)`: measure input level and return samples, peak, and average percent.
- `capture(frames)`: capture 1 through 4096 native input frames and return
  `(pcm, format)`. `pcm` contains interleaved little-endian signed-16 samples.
  `format` contains `sample_format`, `sample_rate`, `channels`, and
  `bits_per_sample`.
- `loopback(duration_ms[, volume])`: run microphone-to-speaker loopback.
- `wav_info(path)`: inspect a WAV file.
- `record_wav(path, duration_ms)`: record a native WAV file.
- `play_wav(path[, volume])`: play a native WAV file.

Example:

```python
import solaros

print(solaros.audio.status())
solaros.audio.tone(880, 200, 40)
sound = solaros.audio.tone_async(1175, 70)
print(solaros.audio.queue_status())
print(solaros.audio.level(500))
pcm, format = solaros.audio.capture(1024)
print(len(pcm), format)
```

## `solaros.speech`

Available when the offline speech package is compiled. Start its background
worker before submitting text:

```python
solaros.jobs.start("speechd", ["/voices/en-US"])
utterance = solaros.speech.say("Solar O S is ready")
print(solaros.speech.request_status(utterance))
```

- `say(text[, volume[, drop_if_busy]])`: copy and queue up to 512 UTF-8 bytes,
  return immediately, and return a request ID. Volume defaults to the global
  speaker volume. With `drop_if_busy=True`, drop a request rather than wait
  behind speech or another audio owner; use `request_status()` to observe a
  later audio-ownership drop.
- `cancel(request_id)`: remove queued speech or request cancellation of the
  active utterance.
- `request_status(request_id)`: return `id`, `state`, `error`, and
  `error_name`, or `None` after the bounded result history expires.
- `queue_status()`: return running/current state, queue depth and capacity,
  and completion, cancellation, drop, and failure counters.

The selected directory must contain `ta.bin` and `sg.bin`; restarting the job
with another voice directory changes the language. Accepted text belongs to
the native queue and remains valid if the submitting script exits. `job stop
speechd` cancels the queue and frees the PicoTTS engine and voice buffers.

## `solaros.synth`

Available when the firmware includes the synth service. The native engine has
eight voices and renders continuously without running Python in the real-time
audio callback. It uses the system's global speaker volume.

- `status()`: return ownership, both oscillator configurations, amplifier and filter envelopes, mono and glide settings, voice, sample-rate, render-deadline, and captured-PCM telemetry. The `pcm_peak` and `pcm_rms` fields come from the shared DSP service.
- `configure(waveform[, attack_ms[, decay_ms[, sustain_percent[, release_ms]]]])`: configure active voices immediately and set the defaults for future notes. Waveforms are `square`, `triangle`, `saw`, `sine`, and `noise`; envelope times are 0 through 10000 ms and sustain is 0 through 100 percent.
- `configure_oscillator2(waveform[, octave[, detune_cents[, mix_percent]]])`: configure the second oscillator. Octave is -2 through +2, detune is -100 through +100 cents, and mix is 0 through 100 percent. A zero mix is an exact oscillator-1 bypass.
- `configure_performance([mono[, glide_ms]])`: select polyphonic or monophonic last-note playback and set portamento from 0 through 2500 ms.
- `configure_filter(cutoff_hz[, resonance_percent[, envelope_amount_percent[, attack_ms[, decay_ms[, sustain_percent[, release_ms]]]]]])`: configure the resonant low-pass filter. Cutoff is 40 through 18000 Hz; percentages are 0 through 100; times are 0 through 10000 ms.
- `note_on(frequency_hz[, velocity])`: start or retrigger a note from 20 through 8000 Hz. Velocity defaults to 100 and ranges from 1 through 127.
- `note_off(frequency_hz)`: release the matching note.
- `all_notes_off()`: release all active notes through their configured release envelopes.
- `stop()`: stop immediately and release audio ownership.

The first `note_on()` claims the exclusive audio output lazily. A script also
releases that ownership automatically when it exits or is interrupted.

```python
import solaros

solaros.synth.configure("saw", 5, 80, 65, 140)
solaros.synth.configure_oscillator2("square", 0, 7, 35)
solaros.synth.configure_filter(1200, 35, 80, 5, 250, 20, 180)
solaros.synth.configure_performance(True, 80)
solaros.synth.note_on(440, 110)
solaros.synth.note_on(554, 90)
solaros.time.sleep_ms(250)
solaros.synth.all_notes_off()
solaros.time.sleep_ms(150)
solaros.synth.stop()
```

## `solaros.dsp`

`solaros.dsp` accepts native little-endian signed 16-bit buffers and provides
`backend`, `capabilities`, `dot`, `gain`, `mix`, `clip`, `level`, `window`,
`fir`, and `fft`. Stateless output operations return a new `bytearray`.
Streaming constructors return objects with explicit `reset()` and `close()`
methods. See [Digital signal processing](dsp.md) for the fixed-point contract,
limits, and examples.

## Quick reference

Use `solaros.audio`, `solaros.speech`, `solaros.synth`, `solaros.dsp`, `solaros.controls`, `solaros.parameters`, `solaros.midi`, `solaros.osc` for audio and control.
See `man python` for runtime conventions and service availability.

# Synth Service

`service.synth` is SolarOS's reusable real-time sample-generation layer. It
depends on the device-independent `service.audio`. The selected built-in or
runtime-attached provider continues to own its hardware, global volume, and
exclusive PCM output, while the synth service owns the render worker, block
timing, and telemetry.

## Client contract

A client supplies `solar_os_synth_config_t` with a stable owner name, optional
exact playback-stream ID, render callback, callback context, and a block size
from 32 through 512 frames. An empty playback-stream selection follows the
current default audio output. The
callback receives signed 16-bit interleaved stereo storage and the active output
sample rate. It must fill exactly the requested frame count without blocking or
performing filesystem or network I/O.

Only one client can run at a time. `solar_os_synth_start()` returns
`ESP_ERR_INVALID_STATE` when audio output or another synth client is busy.
The same owner stops the worker with `solar_os_synth_stop()`. Status reports
the owner, selected playback stream, format, rendered frames and blocks, render deadline misses, write
errors, maximum render time, and the last service error.

The worker uses an internal-memory stack and a bounded internal PCM block. It
opens the selected or default playback stream through `service.audio`, renders and writes
blocks until stopped, submits one silent tail block, and then releases the
stream and its provider resources. The stream is opened, written, and closed by
the same task.

## Native voice engine

`solar_os_synth_voice.h` builds a bounded musical voice engine on the callback
layer. It provides eight voices, automatic release-first voice stealing,
per-note velocity, two oscillators with square, triangle, saw, sine, noise, and
custom-wavetable sources, and a unity-safe oscillator balance. Oscillator 2 adds
octave and fine-detune controls. Each voice also has a shared resonant low-pass
filter with cutoff, resonance, envelope amount, and an independent ADSR
envelope. Configuration changes update active voices immediately and also set
the defaults for new notes.
The render path uses fixed-point oscillators and envelopes; scripting runtimes
only submit control changes and never run inside the audio callback. The mixed
voice signal retains the same PCM headroom as the system tone generator before
the codec applies global speaker volume. Periodic oscillators are evaluated at
eight evenly spaced sub-samples per output frame and averaged before mixing.
The custom oscillator reads a service-owned 256-sample signed wavetable with
linear interpolation; complete table updates are copied under the voice lock so
the render callback never reads mutable client memory.
Oscillator 2 is disabled by an exact zero-mix bypass by default. Mix changes,
pitch changes, and waveform changes are ramped or crossfaded on held notes
before both oscillators enter the shared filter and amplifier envelope.
The filter uses a two-pole state-variable topology. Its coefficients update at
a bounded control rate, transitions between dry and filtered output are ramped,
and resonant peaks use the voice mixer's existing output headroom instead of
altering ordinary filtered samples.
The service latches a consecutive 64-sample trace and fingerprint of a complete
final mono PCM block so status reports describe the samples submitted to audio
rather than inferring them from the selected waveform.

`solar_os_synth_voice_note_on()` lazily claims output for its owner. Matching
`note_off()` calls enter the release stage, `all_notes_off()` releases every
voice, and `stop()` immediately stops the worker and gives up audio ownership.
The global audio service remains responsible for speaker volume.
Mono mode uses last-note priority: a new held note takes over the single voice,
and releasing it returns to the most recently held note. Configurable
portamento glides between those pitches without retriggering either envelope.

Python and Lua expose the engine as `solaros.synth`. Their runtime owners are
released automatically on normal exit, error, cancellation, or foreground-app
shutdown, so a script cannot leave an audio stream or sustained note behind.

The native foreground `synth` app turns the voice engine into a playable
instrument. Run `synth --headless` to suppress the graphical interface while
keeping physical and terminal keys, published parameters, control bindings,
and MIDI input active. This permits use on a headless board with a runtime
playback device such as the LEDC PWM audio expansion. The normal graphical
Play tab pairs the waveform selector and live PCM oscilloscope
with an envelope graph, global speaker volume, editable ADSR knobs, and the
physical-key piano. Its Wave tab draws the custom wavetable at full width and
supports selectable 16, 32, and 64-point resolution, starting at 16; square,
triangle, saw, Supersaw, sine, and flat starting shapes; cursor and brush
editing; smoothing; normalization; reset; and undo. `Enter` cycles the resolution and
resamples the current shape into the new point count. Note input remains active
while editing, the graph includes the cyclic last-to-first interval, and table
changes reshape held notes immediately. Switching tabs does not rewrite the
custom wavetable.
The Filter tab pairs a live low-pass response graph with the independent filter
envelope. Cutoff, resonance, envelope amount, and filter ADSR are editable while
note input remains active.
The Oscillator 2 tab shows both sources and provides waveform, octave from -2
through +2, fine detune from -100 through +100 cents, and mix from 0 through
100 percent. Both oscillators use the same custom wavetable when selected.
The Preset tab contains eight read-only factory sounds and eight user slots.
`Enter` loads the selected patch and `V` saves the complete current sound to a
user slot. A patch contains both oscillators, amplifier and filter envelopes,
filter controls, mono/poly mode, glide time, and the custom wavetable;
performance octave, velocity, hold mode, and global speaker volume remain
unchanged. The Glide tab selects polyphonic or monophonic last-note playback,
toggle-style hold mode, and glide from 0 through 2500 ms. With hold enabled,
each physical or terminal piano-key press toggles its note on or off and key
release does not stop it. Disabling hold releases all latched app-key notes.
MIDI Note On, Note Off, and sustain keep their normal behavior. User slots are
stored as versioned,
checksummed files below `.solar/synth/presets` on the preferred persistent
volume, with internal flash used when no SD card is mounted.
All six tabs use the same compact on-screen piano. `X` hides or shows it while
physical note keys and MIDI input remain active. When the piano is hidden, each
tab expands its knobs, graphs, panels, or preset list into the freed space.
`Tab` cycles through Play, Filter, Wave, Oscillator 2, Glide, and Presets.
Number keys `1` through `6` select them in that order.

Display targets smaller than 240 pixels wide or 200 pixels high automatically
use the Synth parameter HUD instead of the full editor. It shows the selected
control as a large value and level bar, gives waveforms and the wavetable their
own graph, and shows one preset slot at a time. Targets smaller than 112 by 56
pixels drop the footer to preserve the control and graph area. Physical note
keys and MIDI remain active in both compact layouts. A successful change
through the shared parameter/control service temporarily focuses the changed
parameter until the next local navigation action, so an appliance knob provides
immediate visual feedback without changing the current tab.
During active playing, compact displays defer visualization until note input is
quiet so synchronous display transfers cannot take priority over note-on and
note-off handling.

The app also accepts note input from the shared MIDI service. MIDI Note On and
Note Off preserve channel and velocity, controller 64 provides sustain, and
controllers 120 and 123 release the notes on their channel. Start the MIDI job
on a named MIDI bus before opening the app. MIDI notes highlight the matching
pitch class on the on-screen piano just like physical note keys:

```text
expansion bus create midi midi0 tx=gpio1 rx=gpio2
job start midi midi0
synth
```

The bus chooses an available UART controller internally and uses the standard
31250 baud rate unless `baud=` is supplied. The controller number is visible
in status output for diagnostics but is not part of the MIDI bus interface.

While it is running, the app publishes stable native continuous parameters for
volume, amplifier ADSR, filter cutoff/resonance/envelope ADSR and amount,
oscillator 2 octave/detune/mix, and glide. `control parameters` lists their
current ranges and values. A physical ADC knob can control the cutoff without
synth-specific input code:

```text
control create cutoff adc1 0 3300 smooth=40 deadband=8
control bind cutoff parameter synth.filter.cutoff pickup=on
job start controls
synth
```

The cutoff parameter declares a logarithmic 40 through 18000 Hz curve, so a
linear physical potentiometer has musically useful travel across the audible
range. Parameters disappear while Synth is suspended or stopped; bindings stay
configured and reconnect when Synth resumes.

An incoming MIDI controller can use the same parameter path through an explicit
scalar stream:

```text
midi monitor
midi stream add 1 74
control create cutoff midi.cc.1.74 0 127
control bind cutoff parameter synth.filter.cutoff pickup=off
job start midi midi0
job start controls
synth
```

The MIDI service retains the latest matching CC value while its job runs, and
the controls service applies the Synth parameter's logarithmic curve. Use the
monitor first to discover the controller's channel and CC number.

The app also shows current octave and velocity, active voices, sample rate, and
audio errors. Keyboard press and release events sustain held notes and support
chords. Waveform and envelope edits keep oscillator phase and pitch continuous.
After a note renders, the waveform panel shows the captured PCM trace with
automatic vertical scaling and the low 16 bits of its block fingerprint.
Python and Lua synth status return the same fingerprint, range, mean absolute
level, and trace samples.

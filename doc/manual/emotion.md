+++
id = "emotion"
title = "Emotion engine"
section = "api"
summary = "Animated facial expressions and optional reaction sounds for Python and Lua"
keywords = "emotion face robot robotics eyes expression animation gaze blink chirp native library"
packages_any = ["service_emotion"]
agent_reference_sections = true
+++
# Emotion engine

The emotion engine draws procedural eyes on a supported display target and
can play short reaction sounds. A script chooses the expression; a separate
worker advances the animation while the script performs other work.

The optional `emotion` native library requires ESP32-S3 with executable PSRAM.
The firmware must include the emotion runtime and Python or Lua. The standard
Python and Lua package groups select the runtime on compatible boards. Sound
also requires the audio service and an available output device.

## Start a face

```text
pkg available
pkg install emotion
display list
```

Install `emotion` when it is available in the configured signed module catalog.
Opening a face loads the installed library; it does not download code.

```python
import solaros

with solaros.emotion.open(sound=True) as face:
    face.set("happy", transition_ms=200)
    face.look(x=0.4, y=-0.2)
    face.react("surprised", hold_ms=700, sound="chirp")
    solaros.time.sleep_ms(2000)
```

From a port shell or a headless script, supply a ready target returned by
`display list`, for example `solaros.emotion.open("oled0")`.

## Display and lifetime

`open(display=None, sound=False, fg=None, bg=None, fps=0, motion=None)` returns a
face object. Python accepts positional or keyword arguments. Lua accepts the
same arguments positionally; use `nil` for omitted optional values.

- Omit `display` to use the calling foreground application's display. An
  explicit name matching that display uses the same foreground lifetime.
- A different target is claimed exclusively until the face closes. A busy or
  absent target raises an error; the engine does not replace another owner.
- A foreground face pauses its animation clock and stops its sound when its
  session is suspended. It restores its frame on resume. A face on a separately
  claimed target continues until closed or its owning interpreter exits.
- `close()` stops the worker and presenter before releasing the display and
  library. Repeated successful closes are harmless. If shutdown reports a
  timeout, the instance retains its resources and can be closed again.
- Interpreter teardown closes remaining faces after normal completion,
  exceptions, or cancellation. Use Python's `with` or Lua's `<close>` for
  a shorter, explicit lifetime.

At most four faces may be open across all consumers, with one instance per
display target. Each face has independent expression and gaze state. Close
faces before using the script graphics or TUI APIs. Drawing over an active face
is not supported. The library cannot be unloaded while a face holds it.

## Appearance and refresh

When the face opens, it uses the configured terminal foreground color for the
eyes and background color for the background (`setterm foreground` and
`setterm background`). Optional `fg` and `bg` values override those colors for
this face. Each accepts an integer from `0x000000` to `0xFFFFFF` in `0xRRGGBB`
format; omitted values or `None`/`nil` independently use the terminal setting.
The colors are captured at open; reopen the face to change them. Monochrome
targets convert the selected colors through the display service.

```python
face = solaros.emotion.open(fg=0x00FFFF, bg=0x000000)
```

The equivalent Lua call is
`solaros.emotion.open(nil, false, 0x00FFFF, 0x000000)`.

Faces preserve their proportions and fit the target's oriented dimensions.
`fps=0` uses the target's preferred rate, capped at 25 frames per second. An
explicit value accepts 1..30 and is capped by the same runtime and target
limits. This is a submission limit, not a guarantee of physical refresh speed.

`motion=None` enables idle movement and automatic blinking when the target
advertises at least 20 frames per second. Slower targets default to reduced
motion and at most two submissions per second. `motion=False` selects that
mode explicitly; `True` enables animation within the target's rate limit.
Unchanged images are not resubmitted. Short transitions or blinks may be
skipped by slow displays; use longer-held poses on e-paper.

SSD1306 and SH1106 expansion displays advertise 25 fps, so automatic motion is
enabled by default. Actual refresh speed depends on the named I2C bus speed
and other devices sharing that bus. Animation follows elapsed time and skips
intermediate images when presentation takes longer than a frame interval.

## Face methods

| Python method | Behaviour |
| --- | --- |
| `set(pose, transition_ms=200)` | Change the persistent expression and cancel the current reaction and sound. |
| `react(pose, hold_ms=700, transition_ms=120, sound=None)` | Transition to a temporary pose, hold it, then return to the persistent expression over 200 ms. A new reaction replaces the previous one. |
| `look(x=0, y=0)` | Set gaze offsets from -1.0 to 1.0. Negative y looks up. Gaze remains independent of expression. |
| `blink()` | Request a 180 ms blink. |
| `status()` | Inspect dimensions, rate, motion/audio availability, frame counters, and output errors. |
| `close()` | Stop and release this instance. |

Durations accept 0..60000 ms. `set`, `react`, `look`, and `blink` enqueue work
without waiting for presentation. Each instance holds up to eight pending
commands; a full queue raises a timeout error. Pending commands are processed
in order. Commands sent during foreground suspension wait for resume within
that same queue limit. Opening a new foreground face while suspended raises
an invalid-state error.

`status()` returns `width`, `height`, `fps`, `motion`, `sound`, `frames`,
`dropped`, `audio_skipped`, `error`, and `audio_error`. `frames` counts accepted
frame submissions, not physical display refreshes. `dropped` counts rejected
submissions. Error fields retain the last reported error; audio errors reset
when a later cue successfully opens the output.

## Poses and sounds

The 21 named poses are:

```text
neutral       blink_high    happy         glee          blink_low
sad_down      sad_up        worried       focused       annoyed
surprised     skeptic       bored         unimpressed   sleepy
suspicious    squint        angry         furious       scared
awe
```

The pose names describe the supplied Cozmo-style eye reference. Gaze and
blinking can also be controlled independently. The engine uses authored
geometry and timing rather than stored animation images.

Reaction sounds are `chirp`, `giggle`, `sigh`, and `alert`. Omit `sound`, pass
`None`/`nil`, or use `"silent"` for a silent reaction. Sound must also be enabled
when opening the face. Ordinary expression and gaze changes do not produce
sound. Synthesized cues respect global volume and release the output after
playback. If another consumer owns the speaker, the cue is skipped while the
face continues; inspect `audio_skipped` and `audio_error`.

## Lua example

```lua
local solaros = require("solaros")
local face <close> = solaros.emotion.open(nil, true)
face:set("happy", 200)
face:look(0.4, -0.2)
face:react("surprised", 700, 120, "chirp")
solaros.time.sleep_ms(2000)
```

Lua uses the same face methods with colon syntax and positional arguments.
For a dedicated display, replace `nil` with a verified target name.

## Native consumers

Native applications and jobs can acquire library `emotion`, ABI 1, through
the [native library service](native-library.md), using
`solar_os_emotion_abi.h`. The engine accepts caller-owned state, INDEX2 output
buffers, and monotonic timestamps. It also generates mono S16 sound samples.
Native consumers own their scheduling and use their available host services
for presentation and playback. The library itself acquires no hardware.

A foreground application can use its existing foreground video/audio service.
Use its `text_colors` method to obtain the terminal foreground and background
in RGB565. `solar_os_emotion_palette_rgb565` builds the four-entry presentation
palette, with background at index 0 and eyes at index 3. Supply different
RGB565 endpoints to override either color:

```c
uint16_t fg, bg, palette[4];
int err = foreground->text_colors(&fg, &bg);
if (err != SOLAR_OS_NATIVE_OK) return err;
/* Optional override: fg = 0x07ff; bg = 0x0000; */
solar_os_emotion_palette_rgb565(fg, bg, palette);
```

Pass this palette to the native foreground video service with INDEX2 frames.
Firmware consumers of `solar_os_emotion_open` can set `foreground_set` and/or
`background_set` alongside the corresponding `foreground_rgb888` and
`background_rgb888` options; unset colors use terminal settings.

The scripting runtime adapter is a firmware service; it is not a general
display/audio ABI for external jobs. An independent emotion job is not required
to use a face from a robot script.

## Quick reference

Requires ESP32-S3, executable PSRAM, the emotion runtime, Python or Lua, and
the installed `emotion` native library. `pkg available` lists catalog modules;
`pkg install emotion` installs it when published. `display list` lists targets.

```text
solaros.emotion.open(display=None, sound=False, fg=None, bg=None, fps=0, motion=None)
face.set(pose, transition_ms=200)
face.react(pose, hold_ms=700, transition_ms=120, sound=None)
face.look(x=0, y=0)
face.blink()
face.status()
face.close()
```

Lua uses positional arguments, `nil`, colon methods, and `<close>`; Python
supports keywords and `with`. Omit the display only in a foreground application.
Use a verified explicit target for port shells and background scripts.

`fg` colors the eyes and `bg` colors the background. Both accept `0xRRGGBB`
integers; omitted values use the terminal colors at open. Sound cues:
`silent`, `chirp`, `giggle`, `sigh`, `alert`; sound also needs `sound=True` and
an available audio output. A busy speaker skips the cue.

Gaze is -1..1, negative y up. Durations are 0..60000 ms. `fps=0` selects the
target rate; 1..30 requests a rate, capped at 25 and the target preference.
Automatic motion is enabled only for targets advertising at least 20 fps;
reduced motion is limited to 2 fps. Slow panels can skip brief transitions.

Four instances maximum, one per target, eight queued commands per instance.
Foreground faces pause with their session; dedicated displays continue.
Close faces before graphics/TUI drawing. Close joins workers before releasing
ownership and the library; a shutdown timeout retains resources for retry.
Interpreter exit closes remaining faces. Frame counters report submissions,
not physical refreshes. Native consumers use `solar_os_emotion_abi.h`, ABI 1.

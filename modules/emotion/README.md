# Emotion library

Optional ESP32-S3 native library for procedural facial expressions and short
synthesized reaction sounds. It requires the SolarOS native-module service and
executable PSRAM. It contains authored eye geometry, without image or audio assets.

The versioned C contract is `include/solar_os_emotion_abi.h`. Acquire library
`emotion`, ABI 1, through the native `library` service. Allocate `state_bytes`
with normal heap alignment, call `init`, and serialize calls on each state.
Keep the library lease until all state users have stopped. The library starts
no workers and acquires no hardware; different states are independent.

`set` changes the persistent pose. `react` temporarily replaces it, then returns
to it after the transition and hold. A new reaction replaces the previous one;
`set` cancels the reaction. `look` accepts -1000..1000 on each axis. Negative y
looks up. `blink` closes and opens the eyes over 180 ms. Pass monotonic
milliseconds to state operations. Timelines advance correctly when frames are
skipped. Hosts may pause their clock during suspension.

`render` writes INDEX2 pixels, least-significant pixel first, with background
index 0 and eye index 3. It preserves aspect ratio and clears row padding.
The SolarOS host defaults to terminal foreground/background colors, with
optional per-face overrides. Native consumers can build the presentation
palette with `solar_os_emotion_palette_rgb565(foreground, background, palette)`.
Set `animate=0` to suppress idle movement and automatic blinks.
`sound` produces chunk-independent mono S16 samples for chirp, giggle, sigh,
and alert cues. Playback, volume, and output ownership belong to the host.

Build with the ESP-IDF environment used for other external modules:

```sh
idf.py -B ../../.pio/emotion-module set-target esp32s3
idf.py -B ../../.pio/emotion-module build
cmake --build ../../.pio/emotion-module --target typed_elf
```

The artifact is `solaros_emotion.library.elf`. The top-level `module.toml` is
consumed by the normal signed module catalog build. Installing a library does
not start an expression or claim a display.

Python and Lua consumers use the firmware adapter described in the
[emotion manual](../../doc/manual/emotion.md). The runnable pose tours are
[Python](../../examples/python/emotion.py) and [Lua](../../examples/lua/emotion.lua).

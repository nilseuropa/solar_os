+++
id = "python.media"
title = "Python camera, streams, and RTSP API"
section = "api"
summary = "Owned local stream frames, camera snapshots, native images, and asynchronous RTSP sessions"
keywords = "python camera snapshot streams jpeg rtsp rtp media image rgb565"
packages_any = ["app_python"]
agent_reference_sections = true
+++
# Python camera, streams, and RTSP API

[API overview](python.md) · [Lua media API](lua.media.md)

## Quick reference

- Local sources: `solaros.streams.list()`, `open(id[, options])`,
  `acquire_frame(handle)`, `release_frame(frame)`, and `close(handle)`.
- Still images: `solaros.camera.snapshot([size[, quality[, source]]])`
  or `capture(path[, size[, quality[, source]]])`.
- Native decode/presentation: `solaros.image.from_frame(frame)`,
  `decode(data)`, `present(image, x, y[, width, height])`, and `close(image)`.
- QR detection: `solaros.vision.qrcodes(image[, options])` when `service.vision`
  is compiled; see [Computer vision](vision.md) for payloads, regions, and results.
- Network receiver: `solaros.rtsp.open(url[, video[, audio]])`,
  `status(handle)`, `read_frame(handle[, timeout_ms])`,
  `lateness(handle, frame)`, and `close(handle)`.
- Handles belong to one runtime. Release promptly; teardown also closes them.

## Local streams

- `solaros.streams.list()`: registered stream metadata.
- `info(id)`: registry metadata and the provider's default format.
- `status(handle)`: metadata with the opened handle's actual direction and
  negotiated audio/video format.
- `open(id[, options])`: acquire an interpreter-owned source/sink handle.
- `close(handle)`, `close_all()`: release held frames, sources, and RTSP sessions.
- `read(handle, bytes[, timeout_ms])`: bounded byte/PCM read, at most 16384 bytes.
- `write(handle, data[, timeout_ms])`: bounded byte/PCM write; returns bytes written.
- `read_scalar(handle[, window_ms[, timeout_ms]])`: one numeric sample.
- `acquire_frame(handle)`: lease one compressed JPEG frame from a video source.
- `frame_info(frame)`: codec, width, height, length, timestamp_us, and network flag.
- `frame_data(frame)`: explicitly copy JPEG bytes into the interpreter heap.
- `frame_save(frame, path)`: write JPEG bytes directly to storage.
- `release_frame(frame)`: return the native frame.

Open options use a dictionary/table. All streams accept `direction`
(`source`, `sink`, or `duplex`) and `timeout_ms`. Default direction is
source except for sink-only devices. Video accepts `width`, `height`, and
`jpeg_quality` (0 selects the provider default; otherwise 1..63).
Audio accepts `sample_rate`, `channels`, and `frames_per_block`; other
audio properties come from the registered S16_LE format. Unsupported keys are
rejected. Requested formats must be supported by the provider; there is no
automatic resampling or format conversion.

There are four local handles per interpreter and one leased frame per handle.
Frames are limited to 512 KiB. IDs are opaque integers, not pointers; released
or closed IDs cannot be reused or passed to another runtime. Acquisition holds
the same exclusive camera ownership as native consumers. Inspect `info(id).owner`
before diagnosing a busy source. An active `rtspd` or `cam-webd` camera lease
is not bypassed by scripts.

Byte reads poll cancellation between native waits of at most 50 ms. Timeouts
are 0..60000 ms, with a 1000 ms read/write/open default. Reads may return a
partial block or empty data; native timeout failures raise an error.
Writes and scalar operations check cancellation before their bounded native
operation. Camera acquisition uses the camera service's bounded capture wait.
Event streams use their provider's byte-read representation; no generic event
schema is invented here.

## Camera convenience

Available when `service.camera` is compiled:

- `solaros.camera.status()`: backend, sensor, owner, initialized/leased flags,
  selected size/quality, capture count, and last error name.
- `snapshot([size[, jpeg_quality[, source]]])`: return a native JPEG frame
  handle. Defaults are `qvga`, quality 12, and `camera0`.
- `capture(path[, size[, jpeg_quality[, source]]])`: capture directly to a
  JPEG file and release the temporary source, including on write failure.

Sizes are `qvga` (320x240) and `vga` (640x480). A snapshot owns a temporary
camera source until `streams.release_frame(frame)`. For repeated captures,
open the stream once and acquire/release frames in a loop instead of restarting
the camera each time. Files are overwritten by capture/frame_save.

File-save failures include the resolved path and filesystem cause in an
`OSError`, with the numeric errno in `error.args[0]` (for example, 2 for a
missing parent directory). Capture releases its temporary camera source before
raising; a failed `frame_save` leaves the caller's frame leased for retry/release.

## Images without temporary files

When `media.image` is compiled:

- `solaros.image.decode(data)`: decode compressed JPEG/PNG/GIF/WebP bytes into
  a native PSRAM-backed image handle; the input is borrowed only during decoding.
- `from_frame(frame)`: decode a leased camera or RTSP JPEG without copying its
  compressed bytes into the interpreter heap. The resulting image is independent
  of the frame and remains valid after frame release.
- `to_rgb(image[, width, height])`: return an owned packed RGB888 `bytearray`,
  optionally resized with nearest-neighbor sampling. Supply both dimensions;
  the copy remains valid after image closure and must fit the interpreter heap.
  This provides pixel buffers for model-specific normalization and quantization.
- `draw(image, x, y[, width, height])`: existing clipped canvas drawing.
- `present(image, x, y[, width, height])`: queue a native presentation.
  On RGB565-capable color displays this bypasses indexed palette conversion;
  other displays use the existing canvas path. The rectangle must fit the display.

Call `gfx.begin()` or `gfx.begin(target)` first. Present does not require a
subsequent `gfx.present()`. Direct RGB565 pixels are not retained in the indexed
canvas; a later canvas refresh can overwrite them. Draw chrome first and present
video afterwards. The RGB565 cache is allocated lazily in PSRAM and released with
the image. GUI-queue presentation failures are reported in the native log.

Image handles retain the existing 16-handle limit, 4 MiB encoded-input limit,
and 2-million-pixel decode limit. Close images promptly; do not queue a growing
history of video frames. Queued draws/presents retain their own image reference.

## RTSP sessions

Available when `service.rtsp-client` is compiled:

- `solaros.rtsp.open(url[, video[, audio]])`: start an asynchronous native
  session; video and audio default to true.
- `status(handle)`: negotiated/playing/ended flags, selected tracks, audio
  format, receive/drop counters, error code/name, and causal `error_detail`.
- `read_frame(handle[, timeout_ms])`: lease one completed JPEG frame or return
  no value when none is available. Default timeout is zero; maximum is 60000 ms.
- `lateness(handle, frame)`: signed microseconds relative to the native
  presentation clock. Negative means early; positive means late.
- `close(handle)`: cancel, release any held video frame, stop native audio,
  and wait for the network worker before freeing its state.

One RTSP session is allowed per interpreter. At least one track must be enabled.
The session uses the existing UDP RTP/JPEG and L16 receiver, RTCP timing, bounded
audio jitter buffer, selected audio output, and global volume. Audio is played
natively, not returned as PCM to the script. Audio-only sessions are supported
with `open(url, false, true)` (Python uses `False` and `True`).

Open returns before negotiation completes. Check `status().ended` and
`error_detail` to distinguish failure from an ordinary frame-read timeout.
An ended audio-only session also returns no video frame. Use explicit
`apps.launch("rtsp", ...)` if you want the complete native viewer/oscilloscope
instead of managing a session.

There is one native compressed video buffer; reception drops frames while it
is leased. Decode promptly, use `lateness` for scheduling or late-frame drops,
and release the frame. RTSP frame metadata includes `rtp_timestamp` (90 kHz)
and monotonic `arrived_us`; its `timestamp_us` is arrival time, not sender
wall-clock time. Local camera timestamps are capture timestamps.

Current RTSP restrictions are unchanged: trusted LAN, UDP media, JPEG/L16,
IPv4/hostnames, and no URL credentials/authentication or TCP-interleaved media.
Interpreters are never called from network/audio tasks.

## QR detection

`solaros.vision.qrcodes(image[, options])` processes an existing native image
handle. Release a camera/RTSP frame after `image.from_frame` and before detection.
Results contain binary `bytes` payloads and original-image corner coordinates;
they remain valid after the image is closed. Processing is limited to 640 by
480 and eight decoded codes. See [Computer vision](vision.md) for crop/resize options,
result fields, cancellation, and examples.

## Image processing

`service.imlib` adds native histogram/statistics, binary thresholding/inversion,
mean/Gaussian/median filters, erosion/dilation/opening/closing, image difference,
and grayscale/LAB blobs under `solaros.vision`. Transformations return new image
handles; source pixels stay unchanged. See [Computer vision](vision.md).

## Model inference and background pipelines

`solaros.inference` loads resident models and checked bundles on ESP32-S3
with PSRAM. Use `load_bundle` and `run_bundle` for native image preparation
and built-in classification or PICO detection results. Raw models expose
named tensor inputs and outputs. See [Model inference](inference.md) and
[Model bundles](model-bundles.md) for the shared Python/Lua contract.

`solaros.pipeline` runs QR or model processing as an OS-owned background job
over stored images, camera streams, or JPEG RTSP feeds. Scripts can configure
a job, exit, and read its latest structured result from another session.
See [Native image pipelines](pipelines.md) for installation, camera ownership,
and a complete Python detection example. Explicitly stop or destroy pipelines
and unload models when finished; interpreter teardown leaves them resident.

## Cleanup

Explicitly release frames and close sources/images/sessions on all paths.
Interpreter teardown also cleans up on normal exit, cancellation, errors, and
app handoff. Cleanup waits for native RTSP workers; it never force-deletes a
worker using live buffers. If a driver repeatedly fails teardown, SolarOS logs
that failure and retains live lease state rather than freeing memory under it.
Optional camera/image/RTSP modules follow their native service package gates;
adding scripting does not force those hardware/network packages into a build.

## Examples

```python
import solaros

frame = solaros.camera.snapshot()
try:
    print(solaros.streams.frame_info(frame))
    solaros.streams.frame_save(frame, "/flash/snapshot.jpg")
    picture = solaros.image.from_frame(frame)
finally:
    solaros.streams.release_frame(frame)

solaros.gfx.begin()
try:
    solaros.image.present(picture, 0, 0, min(160, solaros.gfx.width()),
                          min(120, solaros.gfx.height()))
    solaros.time.sleep_ms(2000)
finally:
    solaros.image.close(picture)
    solaros.gfx.end()
```

An audio-only receiver without handing control to another app:

```python
receiver = solaros.rtsp.open("rtsp://192.168.1.192:8554/music", False, True)
try:
    while not solaros.should_exit():
        state = solaros.rtsp.status(receiver)
        if state["ended"]:
            print(state["error_detail"])
            break
        solaros.time.sleep_ms(100)
finally:
    solaros.rtsp.close(receiver)
```

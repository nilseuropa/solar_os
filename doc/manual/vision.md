+++
id = "vision"
title = "Computer vision"
section = "api"
summary = "Native QR decoding, statistics, segmentation, filtering, morphology, image difference, and blobs"
keywords = "vision qr qrcode image camera rtsp crop resize imlib histogram statistics filters morphology blobs difference"
packages_any = ["service_vision", "service_imlib"]
agent_reference_sections = true
+++
# Computer vision

[Python media API](python.media.md) · [Lua media API](lua.media.md)

## Quick reference

- Enable the `vision` group (`service.vision` and `service.imlib`) on a board with PSRAM. The
  `full` flavor enables it on supported boards. A camera or display is optional.
- `solaros.vision.qrcodes(image[, options])` reads a native `solaros.image`
  handle and returns QR payloads, four corners, and processing statistics.
- Options: `x`, `y`, `width`, `height`, `output_width`, `output_height`.
- Processing dimensions are limited to 640 by 480. At most eight decoded codes
  are returned. `truncated` indicates additional decoded codes were omitted.
- Python payloads are `bytes`; Lua payloads are binary strings. Inspect `eci`
  before interpreting a payload as text.
- Decode a camera/RTSP frame with `image.from_frame`, then release the frame
  before QR processing. Close the decoded image when finished.
- `service.imlib` adds the image operations below. Each package can be selected
  independently. Native code and Python/Lua use the same backend.

## Images and regions

Use `solaros.image.open(path)` for a stored JPEG, PNG, GIF, or WebP image, or
`solaros.image.from_frame(frame)` for a leased camera/RTSP JPEG. QR processing
uses the decoded image without modifying its pixels. The image remains valid
after processing and can also be drawn or presented.

`x` and `y` select the crop origin, defaulting to zero. Zero or omitted `width`
and `height` select the remaining image from that origin. The crop must fit
within the image. Zero or omitted output dimensions select the crop dimensions.
Larger images require an explicit crop or resize to fit the processing limit.
Resize uses nearest-neighbour sampling; preserve aspect ratio for best results.

Corners are reported in original-image coordinates, including the crop offset
and inverse resize. Their order follows the QR orientation: top left, top right,
bottom right, bottom left. For mirrored codes, orientation reflects the decoder's
original corner order. Python uses tuples of `(x, y)` pairs; Lua uses arrays of
two-element arrays. Payloads do not execute any action automatically.

## Results and limits

The returned dictionary/table contains:

| Field | Meaning |
| --- | --- |
| `codes` | Decoded QR entries; empty when no valid code is found |
| `width`, `height` | Original image dimensions |
| `processed_width`, `processed_height` | Dimensions passed to recognition |
| `candidates` | Recognized QR candidates, including unsuccessful decodes |
| `decode_failures` | Candidates whose payload could not be decoded before the result limit |
| `truncated` | A further valid code was found after eight results |
| `preprocess_us` | Recognition-buffer allocation and grayscale conversion time |
| `detect_us` | Recognition time, including cooperative scheduling |
| `decode_us` | Payload decode and result-copy time |
| `elapsed_us` | Total native processing time, excluding source decode and task launch/cleanup |

Each QR entry contains `payload`, `corners`, `version`, `ecc_level`, `data_type`,
and `eci`. Payload bytes and metadata are copied into the interpreter and remain
valid after the image is closed. ECC values are M=0, L=1, H=2, Q=3; data types
include numeric=1, alphanumeric=2, byte=4, and Kanji=8.

Only one QR request runs at a time. A competing request returns a busy error.
Allocation failures return an error without falling back to internal RAM for
QR buffers. Processing uses a five-second cooperative deadline and checks app
cancellation. The call waits for the worker to stop before releasing its memory;
the deadline is not a hard preemption guarantee for an individual decode step.
The native worker does not call an interpreter.

## Python example

```python
import solaros

picture = solaros.image.open("/qr.jpg")
try:
    result = solaros.vision.qrcodes(picture)
    for code in result["codes"]:
        print(code["payload"], code["corners"])
finally:
    solaros.image.close(picture)
```

For a camera frame, retain the capture timestamp separately so that a result can
be associated with its source:

```python
frame = solaros.camera.snapshot()
try:
    timestamp = solaros.streams.frame_info(frame)["timestamp_us"]
    picture = solaros.image.from_frame(frame)
finally:
    solaros.streams.release_frame(frame)

try:
    result = solaros.vision.qrcodes(picture)
    print(timestamp, result["codes"])
finally:
    solaros.image.close(picture)
```

For repeated captures, open the source once, acquire/decode/release one frame,
process it, close its image, and call `solaros.time.sleep_ms` between iterations.
Do not collect an unbounded queue of frames or decoded images. A script cannot
open the exclusive local camera while `rtspd` or `cam-webd` owns it.

## Lua example

```lua
local picture = solaros.image.open("/qr.jpg")
local ok, result = pcall(solaros.vision.qrcodes, picture)
solaros.image.close(picture)
if not ok then error(result) end
for _, code in ipairs(result.codes) do
    print(code.payload, code.corners[1][1], code.corners[1][2])
end
```

The `vision` module follows its native package gate. Enabling Python or Lua
alone does not enable vision. Neural inference is not included in this API.

## Image processing with imlib

The selected OpenMV imlib routines accept existing `solaros.image` handles.
Source images remain unchanged. Transformations return new image handles;
close them through `image.close`. Analyses return owned dictionaries/tables.
Interpreter exit closes its image handles. Stored pictures and decoded stream
frames use the same API.

Python and Lua expose the same positional calls:

| Call | Return |
| --- | --- |
| `vision.histogram(image[, options])` | Normalized histogram fractions in `channels` |
| `vision.statistics(image[, options])` | Region statistics in `channels` |
| `vision.binary(image, options)` | New thresholded black/white image |
| `vision.invert(image[, options])` | New image with inverted pixel values |
| `vision.mean(image[, options])` | New image with a box filter |
| `vision.gaussian(image[, options])` | New image with a Gaussian filter |
| `vision.median(image[, options])` | New image with a median filter |
| `vision.erode(image[, options])` | New image with erosion |
| `vision.dilate(image[, options])` | New image with dilation |
| `vision.opening(image[, options])` | New image, erode then dilate |
| `vision.closing(image[, options])` | New image, dilate then erode |
| `vision.difference(image, reference[, options])` | New absolute per-channel difference image |
| `vision.blobs(image, options)` | Connected regions in `blobs` |
| `vision.process(image, operation[, options[, reference]])` | Analysis or transformation record with native timing and scratch usage |

`operation` uses the table's function names, such as `"gaussian"` or `"blobs"`.
For transformations, `process` returns the new handle in `image`. Difference
requires a reference with the same original dimensions as the source; both
images receive the same crop/resize.

### Options

Options are a Python dictionary or Lua table. Unknown keys, invalid types,
out-of-range values, and options belonging to another operation fail before
worker execution.

| Common option | Meaning |
| --- | --- |
| `format` | `"gray"` default, or `"rgb565"` for color processing |
| `x`, `y`, `width`, `height` | Source crop; zero dimensions select the remaining image |
| `output_width`, `output_height` | Nearest-neighbour resize; zero selects crop size |
| `timeout_ms` | Cooperative deadline, 1–60000 ms; default 5000 |

Processed dimensions must fit 640 by 480. Larger pictures require an explicit
crop or resize. A transformed image contains the processed crop at the selected
output size. Processing uses grayscale or RGB565 temporary pixels and copies
transformed pixels into a new immutable RGB888 SolarOS image. Color processing
therefore includes RGB565 quantization.

- Histogram/statistics: optional `bins` (2–256). Defaults are 256 for grayscale,
  101 for LAB lightness, and 256 for each LAB chromatic channel. Fewer bins make
  statistics approximate. Optional `thresholds` and Boolean `invert` restrict
  measurements; an empty selection produces zero histogram fractions.
- Binary/blobs: required `thresholds`, up to eight inclusive ranges. Grayscale
  uses `[min,max]` in 0–255. RGB565 uses `[Lmin,Lmax,Amin,Amax,Bmin,Bmax]`,
  with L in 0–100 and A/B in -128–127. Boolean `invert` inverts each range's match.
- Filters/morphology: `ksize` is the kernel radius, 1–3, default 1. A radius of
  one selects a 3 by 3 neighborhood. Median uses the middle percentile;
  Gaussian uses Pascal coefficients. Threshold an image first for binary-mask
  morphology. Filters retain the upstream edge handling.
- Blobs: `x_stride`, `y_stride` (default 1), `area_threshold` and
  `pixels_threshold` (default 10), Boolean `merge` (default false), `margin`
  (0–640, default 0) and `max_blobs` (1–64, default 32). Area checks the bounding
  box; pixel threshold checks matching pixels.

### Results and resources

Measurements contain `channels.gray`, or `channels.l`, `channels.a`, and
`channels.b` for RGB565/LAB. Histogram channels are arrays of fractions.
Statistics channels contain `mean`, `median`, `mode`, `stdev`, `min`, `max`,
`lower_quartile`, and `upper_quartile`.

Blobs contain `x`, `y`, `width`, `height`, `cx`, `cy`, `rotation` in radians,
`pixels`, `code` (threshold bitmask), and `count` (merged components). Bounds
and centroids use original source-image coordinates, including crop/resize.
Pixel count uses the processed resolution. Rotation is mapped back using both
resize scale factors. The record includes `coordinates="source_pixels"` and
`truncated`; truncation means the output limit or the 512-candidate admission
limit omitted regions. Candidate admission happens before merging.

Records also include `operation`, `format`, source `roi`, `processed_width`,
`processed_height`, `preprocess_us`, `process_us`, `output_us`, `elapsed_us`,
and `workspace_peak_bytes`. Timing excludes image decoding, task launch/cleanup,
and interpreter object construction. Scratch usage includes converted images
and tracked algorithm allocations; it excludes the worker stack, result record,
returned image, and interpreter copies.

One imlib request runs at a time; competing calls report busy. Its 12 KiB worker
stack and scratch exist only while processing. Scratch uses PSRAM, with a 2 MiB
request limit including allocation tracking. Blob flood-fill storage is bounded
to 128 KiB; exhaustion reports an allocation error. Temporary storage is released
on every return. Constant lookup tables occupy firmware flash. Cancellation
and timeout join the worker before releasing buffers. Deadlines are cooperative,
so an individual step can exceed the requested time.

### Python filtering example

```python
from solaros import image, vision

source = image.open("/picture.jpg")
try:
    filtered = vision.gaussian(source, {"output_width": 320, "output_height": 240})
    try:
        mask = vision.binary(filtered, {"thresholds": [[128, 255]]})
        try:
            result = vision.blobs(mask, {"thresholds": [[128, 255]]})
            print(result["blobs"], result["process_us"])
        finally:
            image.close(mask)
    finally:
        image.close(filtered)
finally:
    image.close(source)
```

### Lua color example

```lua
local picture = solaros.image.open("/picture.jpg")
local ok, result = pcall(solaros.vision.blobs, picture, {
    format = "rgb565", output_width = 320, output_height = 240,
    thresholds = {{20, 80, 30, 127, 0, 127}}, pixels_threshold = 30
})
solaros.image.close(picture)
if not ok then error(result) end
for _, blob in ipairs(result.blobs) do
    print(blob.cx, blob.cy, blob.pixels)
end
```

The native pipeline service currently accepts a single QR or model processor.
The examples above sequence synchronous image operations in the interpreter.

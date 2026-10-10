+++
id = "model-bundles"
title = "Model bundles and pipelines"
section = "api"
summary = "Checked model manifests, reusable adapters, and resident file or stream inference"
keywords = "espdl model bundle manifest preprocessing classifier detector pipeline rtsp tensor"
packages_any = ["service_inference"]
agent_reference_sections = true
+++
# Model bundles and pipelines

[Model inference](inference.md) · [Python media](python.media.md)

Use [`zoo`](apps.md#zoo) to browse the hosted catalog and install complete bundles
on SD or flash. Installation verifies the release files; loading and model
residency remain explicit operations through the model service.

For OS-owned background processing with one image input, see
[Native image pipelines](pipelines.md). A pipeline keeps running after its
configuring script exits and exposes a bounded latest result through the shell,
Python, and Lua.

## Quick reference

- Bundles contain `bundle.json`, one `.espdl` file, and declared assets.
- Contract: `schemas/solaros-model-bundle.schema.json`, version one.
- Native Python/Lua API: `solaros.inference.load_bundle` and `run_bundle`.
- Python client helper: `examples/python/model_bundle.py`. Copy it alongside
  your calling script; it is not a built-in module.
- Runner: `examples/python/infer.py`, copied beside `model_bundle.py`.
- Hashes and runtime/port compatibility are checked once when opening a bundle.
- Models belong to the OS and stay resident until explicit unload or reboot.

## Contract and validation

Required fields are `schema=1`, `id`, `version`, `runtime`, `model`, `inputs`,
`outputs`, and `result`. Optional fields are `assets` and `license`. Unknown
fields are rejected. Manifests are limited to 64 KiB. Identity/version strings
contain letters, digits, dots, underscores, or hyphens, at most 100 characters.

`runtime` declares the backend, exact validated version, and target:
`{"backend":"espdl","version":"3.3.13","target":"esp32s3"}`. `model` declares
relative `file` and lowercase hex `sha256`. `assets` maps relative filenames to
hashes, at most 32 entries. `license` contains `id` and `file`, with that file
declared in `assets`. Paths cannot be absolute, contain parent traversal, empty
segments, backslashes, or control characters. Hashes detect content mismatch;
they are not catalog signatures.

`inputs`/`outputs` map exported names to `dtype`, `shape`, and `exponents`,
matching native descriptors exactly. Inputs also declare an `adapter` with
`type` and optional `options`. Shape axes have the meaning declared by the
adapter. Tensor and port limits follow the native service.

Input adapters include `tensor`, passing raw buffers or typed tensor records
directly, and `image`, accepting caller-owned native image handles. The tensor
adapter has no options; image options are described below. Multi-input and
non-image models use the same resident loader.

JSON Schema validates structure. The host builder checks adapter semantics,
sizes, channel axes, detector geometry, output references, and assets. The
native loader independently validates built-in contracts, verifies hashes,
and compares actual model interfaces. It rejects duplicate keys, embedded NUL,
trailing non-whitespace data, and JSON nesting beyond 32 levels.
Numerical validation remains model-specific.

## Native image preparation

Python and Lua expose this call when both image and inference are compiled:

```text
solaros.inference.prepare_image(model, input_name, image [, options])
```

The model port determines output dimensions, type, and quantization.

| Option | Values/default |
| --- | --- |
| `layout` | `NHWC` default, `NCHW`, `HWC`, `CHW`; batch must be one |
| `color` | `RGB` default, `BGR`, `GRAY`; three or one output channels |
| `resize` | `stretch` default, `letterbox`; nearest-neighbor floor sampling |
| `mean`, `std` | One broadcast value or one per output channel; defaults zero/one; finite values and positive standard deviation |
| `pad` | One or per-channel byte before normalization, default zero |
| `x`, `y`, `width`, `height` | Source crop; zero dimensions select the remaining image |

Bundle image adapters declare layout, color, and resize explicitly. Supported
prepared types are int8, uint8, int16, and float32. Integer outputs use
`(pixel-mean)/std/2**exponent`, rounded with `floor(value+0.5)` and saturated.
Zero point is zero. Quantization is per-tensor or per-output-channel, with the
axis determined by layout; supported exponents are -126..126. Float32 uses
`(pixel-mean)/std`. Grayscale uses `(77*R+150*G+29*B)>>8` before normalization.

Letterbox preserves aspect ratio, floors resized dimensions to at least one
pixel, and centers the content. Extra odd padding goes to right/bottom. Sampling,
channel/layout conversion, normalization, and quantization are fused. Temporary
lookup storage uses PSRAM and is freed on every return. Processing yields and
checks cancellation between rows; it does not modify source pixels.

Results contain `data`, `transform`, and `preprocess_us`. Python data is an owned
bytearray; Lua data is an owned binary string. It survives image/model closure
and must fit the interpreter heap. Transform fields report source/input sizes,
crop origin/size, resized sizes, and left/top padding. Inverse coordinates are:

```text
source_x = crop_x + (input_x-pad_left)*crop_width/resized_width
source_y = crop_y + (input_y-pad_top)*crop_height/resized_height
```

`preprocess_us` includes lookup allocation, processing, and native yields;
it excludes decoding and interpreter result allocation/construction. Invalid
options, unsupported geometry/type, allocation failure, and cancellation raise
errors; partially prepared buffers are not returned.

## Result adapters and residency

- `raw`: owned native output tensors, without interpreting their meaning.
- `classification`: one int8/uint8/int16/float32 class vector, checked labels,
  `softmax` or `identity` activation, and configured top-k results.
- `pico-detection`: single-class int8 score/DFL stages with declared tensor names,
  strides, bins, score/IoU thresholds, bounded candidates, and output limits.

Classification payloads contain `kind="classification"` and `classes` with
`id`, `label`, and `score`. Detection payloads contain `kind="detection"`,
`coordinates="source_pixels"`, source dimensions, `truncated`, and `detections`
with `id`, `label`, `score`, and `[x1,y1,x2,y2]`. Boxes use inclusive integer
source coordinates, clipped to source bounds. NMS operates in source coordinates.
`truncated` reports candidate/result limits, not ordinary NMS suppression.
PICO filtering uses its squared-score convention.

```python
from model_bundle import ModelBundle
import solaros

with ModelBundle("/dl/models/imagenet/bundle.json") as model:
    picture = solaros.image.open("/pictures/example.png")
    try:
        result = model.run({"input.1": picture})
        print(result["result"])
    finally:
        solaros.image.close(picture)
```

`run` requires all named inputs and returns owned native `outputs`, native
timings, interpreted `result`, per-input `transforms`, `preprocess_us`,
`postprocess_us`, and `postprocess_ms`. The helper adds `call_ms`, covering
preparation, inference, postprocessing, and binding overhead. Caller image
handles are not closed by adapters. Built-in preparation and result decoding
run natively; prepared tensors and adapter workspace use PSRAM.

`ModelBundle(path)` reuses an existing bundle at that resolved path, or loads
one when absent. `ModelBundle(handle)` attaches to an existing bundle.
`reset()` resets the shared resident model. `close()` explicitly unloads it
and is idempotent for that helper. `detach()` and context exit drop the client
handle without unloading. Script exit and execution errors leave the model
available to other clients; use `model list` to discover it and `model unload`
to release it. Failed native bundle validation never publishes a handle. The helper allows
60000 ms for loading, including hash checks, and 10000 ms for execution by
default; override `load_timeout` and `timeout` separately.

For custom Python adapters, select `ModelBundle(path, native=False)`. This
uses the reference interpreter loader over the same OS-owned raw runtime.
The native loader supports only the built-in adapter types listed above.
Trusted applications can register functions in `INPUT_ADAPTERS` and
`RESULT_ADAPTERS` before loading. Input functions receive
`(bundle,name,value,options)` and return `(buffer,transform_or_none,preprocess_us)`.
Result functions receive `(bundle,outputs,transforms,options)` and return a
payload. Install adapter code separately; manifests do not execute code or
automatically import modules. The raw native runtime does not require adapters.

## File and stream runner

```text
python /dl/infer.py /dl/models/imagenet/bundle.json /pictures/example.png
python /dl/infer.py /dl/models/pedestrian/bundle.json /pictures/example.png --count 5
python /dl/infer.py /dl/models/pedestrian/bundle.json stream:camera0 --count 20
python /dl/infer.py /dl/models/pedestrian/bundle.json rtsp://host:8554/camera --count 20
python /dl/infer.py /dl/models/arithmetic/bundle.json /inputs.json
model list
python /dl/infer.py 1 /pictures/example.png
model unload 1
```

The first argument is a bundle path or an existing bundle handle. The runner
leaves the model resident after completion or cancellation.

Single-image models accept a picture path. For non-image or multi-input models,
an input JSON map names every file, such as `{"a":"a.bin","b":"b.bin"}`.
Relative paths resolve beside that map; absolute storage paths work. Tensor
files are binary and images use native decoding. Files/images are reused for
repeated calls; every image closes afterward.

`--mode single|auto|dual` selects execution. `--count N` limits successful results:
files default to one, streams to unlimited; zero means unlimited for streams.
`--max-age MS` defaults to 250; zero disables filtering. SolarOS permits eight
app launch arguments, so limit the number of options used together.

Streams open once and lease at most one compressed frame. Stale frames are
discarded before decoding. Compressed leases return before preprocessing and
inference; independent decoded images close after each result. There is no
growing frame queue. Up to four consecutive invalid JPEGs are skipped; the fifth
fails and closes the source while keeping the model resident. Local cameras remain exclusive, so a direct
stream cannot open a camera already owned by `rtspd`/`cam-webd`. Received RTSP
video is a separate source.

Stdout contains JSON records with `schema=1`, `event="inference"`, `model_handle`, model identity,
source, sequence, frame metadata, result, transforms, timings, and cumulative
stale/decode-error counts. File records have no frame timestamp. Camera timestamps
are capture time; RTSP timestamps are arrival time, with native RTP metadata
retained. `source_latency_ms` measures time since that timestamp, rather than
sender-to-result RTSP latency. `inference_us` measures model execution;
`preprocess_us` and `postprocess_us` measure native adapters separately. These
timings exclude image decoding and do not establish camera FPS. Raw CLI results contain hex data up to 4096
bytes, otherwise SHA-256 and metadata; the library retains the actual bytes.
The runner does not publish MQTT/OSC or produce annotated video.

## Installing a bundle directory

Copy the complete bundle directory, including its model and declared assets,
to a mounted storage volume. Load its `bundle.json` with `model bundle`,
`solaros.inference.load_bundle`, or the native inference service table. Installing
files does not load the model. A loaded model remains resident until explicitly
unloaded or the device reboots.

Model definitions, zoo metadata, publishing tools, validation results, and
their documentation are maintained in the separate `solar_os_zoo` repository.
SolarOS owns the runtime bundle specification and its native/interpreter APIs.

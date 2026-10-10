# Hardware checks

These tests use live hardware. They do not flash or change persistent settings.
Run with permission to use the relevant hardware resources. RTSP tests occupy
the publisher/viewer and receiver slots.

## Native image pipelines

`pipeline_client.py` starts jobs and reads them from separate Python sessions;
`pipeline_client.lua` inspects the same records from Lua. `native_pipeline.py`
automates binary QR, cat classification, positive pedestrian detection, and
continuous synthetic JPEG RTSP processing after interpreter exit. It checks
monotonic latest-result sequences/timestamps, retained-model unload rejection,
explicit stop, worker release, and explicit model unload. It removes its
temporary device fixtures and stops its private host processes.

Requires Telnet, a temporary FTP job on port 2121, installed model bundles and
the cat/pedestrian sample images described in the harness docstring. It stops
FTP during inference and restarts it only to remove its fixtures.

```sh
python3 tests/hil/native_pipeline.py --telnet DEVICE_IP --host-address HOST_IP \
  --mediamtx /path/to/mediamtx --output /tmp/native-pipeline-evidence
```

SolarTerm validates file and RTSP sources; a local-camera pipeline still needs
a camera-equipped board. Request timing is not an end-to-end camera FPS claim.

## Python RGB export

Upload `image_rgb.py` and a small picture such as
`tests/fixtures/vision/blank.png` to mounted device storage, then run:

```text
python /image_rgb.py /blank.png
```

Requires the image and Python packages. The full RGB picture must fit the
512 KiB Python heap. Checks the actual MicroPython buffer constructor, RGB
length, nearest-neighbor resize coordinates, invalid arguments, allocation
failure and retry, and copy lifetime after closing the native image. A passing
run prints `RGB_PYTHON_OK`. The script releases its image in `finally`.

## Inference execution modes

Upload `inference_modes.py`, a validated integer `.espdl` model with one input,
and its prepared binary input tensor. Run:

```text
python /inference_modes.py /model.espdl /input.bin 3 5
```

Each cycle loads once, compares single/auto/dual execution with one warm-up and
five measured calls per mode, checks identical output hashes, tests deadline
recovery and stale handles, then releases the model. Timings exclude input
preparation, checksums and explicit garbage collection. Observe native `mem`
and `top` before and after interpreter exit to check worker and model release.
Successful completion prints `INFERENCE_MODES_OK`.

## OS-owned inference residency

Install the arithmetic bundle and upload `inference_residency.py` and
`inference_residency.lua` into `/dl`. Run each command separately:

```text
python /dl/inference_residency.py load
model list
python /dl/inference_residency.py run
lua /dl/inference_residency.lua
model list
python /dl/inference_residency.py run
python /dl/inference_residency.py unload
model list
```

The fixture must not already be resident. These checks prove model survival
across interpreter teardown, shared Python/Lua handles and modes, numerical
arithmetic output, explicit unload, and handle discovery. The last Python
command releases only the fixture model. Observe memory/tasks after unloading.

## Native bundle host checks

`make -C tests/host model_bundle_test` builds the real native parser, image
preparation, and result adapters against a fake numerical backend. It requires
OpenSSL development files and the SDK's cJSON source. Override `CJSON_DIR` when
ESP-IDF is installed outside PlatformIO. Run `tests/host/model_bundle_test` to
check hashes, port contracts, allocation-failure rollback, and classifier/PICO
results. `make -C tests/host inference_lua_json_test` uses the same cJSON
source to check actual Lua result conversion and interpreter allocation failures;
run `tests/host/inference_lua_json_test` afterward. These checks do not validate a model's real ESP-DL execution.

## Native image-to-tensor preparation

Upload `image_tensor.py`, `image_tensor.lua`, the reference MobileNetV2 model,
and the classifier's 300×300 `cat.png` sample. Requires image and inference plus
the relevant interpreter. Run:

```text
python /dl/image_tensor.py /dl/models/imagenet/imagenet_cls_mobilenetv2_s8_v1.espdl /dl/cat.png
lua /dl/image_tensor.lua
```

The Lua test uses those model/picture paths directly. Both tests compare every
prepared byte against the interpreter reference, check invalid options and
buffer ownership after image closure, and release the model. The Python test
also checks crop/letterbox geometry and reports a tensor hash and preprocessing
time. Success prints `IMAGE_TENSOR_PYTHON_OK` or `IMAGE_TENSOR_LUA_OK`.

## Loadable native vision services

Build `modules/vision-check`. Requires SolarTerm firmware with the native-module,
image, QR, imlib, inference, script-media and RTSP-client packages, no-auth
Telnet, an existing `/dl` folder, a stopped device FTP job, FFmpeg and MediaMTX.
Host ports 18754/18200/18201 must be free. Pass the host's LAN address.

```sh
python3 tests/hil/native_vision_services.py \
  --telnet 192.168.1.113 --host-address 192.168.1.192 \
  --mediamtx /path/to/mediamtx \
  --module /path/to/solaros_native_vision_check.app.elf \
  --output /tmp/native-vision-services
```

The output directory must not exist. The harness refuses to overwrite its
fixture names and loads the ELF three times. It verifies native image/imlib/QR
results, cancellation, image-to-tensor preparation, raw and bundle inference,
resident handle reuse after ELF unload, and three received RTSP frames per run.
It records memory/tasks, checks worker removal and explicitly unloads its own
models before removing its own files and stopping its FTP job and host feed.
Camera capture is covered by host fake-backend tests, not this RTSP device run.
Success prints `NATIVE_VISION_SERVICES_OK`.

## Native imlib subset

Requires no-auth Telnet, FTP, Python, Lua and `service.imlib`. The device's
`/dl` folder must exist and its FTP job must be stopped. No camera or model is
required.

```sh
python3 tests/hil/imlib_subset.py \
  --telnet 192.168.1.113 --output /tmp/imlib-subset
```

The output directory must not exist. The harness uploads synthetic PNG images
and its Python/Lua clients, refusing to overwrite existing fixture names. It
checks statistics/histograms, thresholds/inversion, filters, morphology,
difference and grayscale/color blobs, including crop/resize coordinates and
input image ownership. It repeats processing, interrupts an active worker and
checks its removal, then records memory/task snapshots. It removes its own
files and stops its temporary FTP job. Success prints `IMLIB_SUBSET_OK`.

## Resident model-bundle RTSP loop

Install `examples/python/model_bundle.py`, `examples/python/infer.py`, and a
checked image-model bundle on the device. Requires no-auth Telnet, FFmpeg,
MediaMTX, and free host ports 18654/18100/18101. Give the host's LAN address.

```sh
python3 tests/hil/model_bundle_stream.py \
  --telnet 192.168.1.113 --host-address 192.168.1.192 \
  --mediamtx /path/to/mediamtx \
  --bundle /dl/models/pedestrian/bundle.json \
  --output /tmp/model-bundle-stream --count 12 --interrupt
```

The output directory must not exist. The harness starts and stops its own
server and synthetic 160×120 JPEG publisher, verifies the finite result count,
source dimensions and increasing frame timestamps, then optionally cancels
active continuous inference. It records JSON results, device memory/task
snapshots, and process logs; success prints `MODEL_BUNDLE_STREAM_OK`. It checks
request/stream worker removal after script exit, checks that the model remains
resident after finite and interrupted runs, and explicitly unloads only its own
model before recording the released snapshot. This proves received-stream inference and cleanup;
the synthetic feed does not validate detector accuracy or local camera capture.

## Native viewer and audio-publisher soak

Requires no-auth Telnet, an idle display shell in session 0, FFmpeg and MediaMTX.
Give the host's LAN address, not loopback. The harness owns its child processes
and temporary files, and quits its viewer/stops its capture job on exit. It does
not stop existing host servers or an already-running `rtspd` job. Host ports
18554/18000/18001 and device port 18555 must be free.

```sh
python3 tests/hil/rtsp_playback_soak.py \
  --telnet 192.168.1.124 --host-address 192.168.1.192 \
  --mediamtx /path/to/mediamtx --cycles 3 --seconds 60 --interrupt --capture
```

Cycles alternate JPEG+L16 and audio-only playback. `--interrupt` alternates
stopping/restarting the publisher and pausing/resuming it to exercise control
EOF, missing paths and the five-second media timeout. Checks include renewed
audio output, connection epochs, live workers and their removal on exit.
`--capture` additionally receives microphone L16 through FFmpeg after playback,
covering publisher negotiation, DMA, reader/control stacks and cleanup.
It does not assert simultaneous capture/playback support.

Use larger `--seconds`/`--cycles` for overnight runs. Save stdout to an evidence
log. Snapshots include internal/PSRAM/DMA free bytes, lifetime low-water marks,
largest free blocks, task minimum-free stack bytes and stream owners. Compare
each released snapshot with the same run's baseline. Heap views overlap; task
request totals in `mem policy` are cumulative, not the active stack budget.
These are observations, not a guaranteed maximum under every firmware workload.

## Native camera publisher pacing

Requires an already-running `rtspd` video publisher and its sole receiver slot
to be available:

```sh
python3 tests/hil/media_rtsp_timing.py rtsp://192.168.1.238/media \
  --seconds 60 --cycles 10 --idle-seconds 2
```

Checks complete-frame throughput, RTP sequence gaps, frame timestamps, initial
backlog, clock drift and RTCP reports across explicit teardown/reconnect cycles.
It does not measure decoded presentation latency or implement a video viewer.

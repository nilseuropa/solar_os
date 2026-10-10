# Native vision service acceptance module

This standalone ESP32-S3 ELF uses only public headers and the versioned
`solar_os_native_host_v1` import. It discovers image, QR, imlib, video,
inference and tensor-image tables at runtime. The firmware must enable the
corresponding packages. Build with an ESP-IDF 5.5 environment:

```sh
idf.py set-target esp32s3
idf.py build
```

The output is `build/solaros_native_vision_check.app.elf`. Copy it and the
acceptance fixtures to storage, then run:

```text
load /dl/vision-check.elf /sdcard/dl/scene.png /sdcard/dl/qr.png /sdcard/dl/arithmetic_int8.espdl /sdcard/dl/bundle.json
```

Use the synthetic scene from `tests/hil/imlib_subset.py`,
`tests/fixtures/vision/multiple.png`, `tests/fixtures/inference/arithmetic_int8.espdl`
and `tests/fixtures/model_bundle/raw.json`. Paths passed to services
are resolved storage paths, unlike the shell's `load` argument.

An optional final argument is a camera stream ID such as `camera0` or a JPEG
RTSP URL. The module decodes three frames and returns each compressed lease
before using the independent decoded image. Camera access remains exclusive.

Checks cover image/result ownership, imlib cancellation, QR payload lifetime,
native image-to-tensor preparation, raw and bundled two-input inference, and
retained-model unload rejection. It frees images/results and destroys clients
before returning. It deliberately leaves its models resident, printing
`NATIVE_MODEL_HANDLE` and `NATIVE_BUNDLE_HANDLE`; explicitly unload those handles
when finished. Repeating the call reuses the same paths and resident models.

Success prints `NATIVE_VISION_CHECK_OK`. The host acceptance harness is
`tests/hil/native_vision_services.py`.

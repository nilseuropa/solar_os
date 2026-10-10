+++
id = "native-media"
title = "Native media and inference"
section = "api"
summary = "Versioned image, camera/RTSP, QR, imlib and resident-model interfaces for ELF modules"
keywords = "native elf module abi image vision imlib qr espdl inference camera rtsp tensor"
packages_any = ["service_native_modules"]
agent_reference_sections = true
+++
# Native media and inference

[Native ELF modules](commands.md#native-elf-modules) · [Computer vision](vision.md) · [Model inference](inference.md)

Native ELF applications and resident job modules discover host-owned services
through a versioned function table. They compile against public headers in
`include/`; these interfaces use standard C types and opaque host objects, with
no ESP-IDF, ESP-DL or interpreter headers. Python/Lua and native clients use the
same underlying services, resident models and processing limits.

## Quick reference

- Discover version-one service tables with the native host's `get_service`.
- Enable each required firmware package and check for NULL before use.
- Release host images/results and destroy clients/sessions before ELF unload.
- Resident models survive client and module teardown until explicit unload.
- Allocate native tensor scratch through the `memory` PSRAM table.

## Discovery and compatibility

Application modules import `solar_os_native_host_v1` from
`solar_os_native_abi.h`. Check `abi_version` and `struct_size` before accessing
the appended `get_service` member. Its presence requires a 24-byte host table
on ESP32-S3; declare `minimum_host_api_size=24` in the module manifest. Older
output-only modules requiring the original 20-byte prefix remain compatible.
Resident jobs use the existing `solar_os_native_job_host_v1().get_service`.

`get_service(name, abi_version, minimum_struct_size)` returns a borrowed,
constant table, or NULL for an unknown name, unsupported ABI, insufficient
table size or disabled package. Discovery allocates no workspace and starts
no workers. Always check availability before calling the table.

| Service name | Public header | Firmware package |
| --- | --- | --- |
| `memory` | `solar_os_native_service_abi.h` | `bootstrap.native-modules` |
| `image` | `solar_os_native_image_abi.h` | `service.image` |
| `media.video` | `solar_os_native_media_abi.h` | `service.script-media` |
| `vision.qr` | `solar_os_native_vision_abi.h` | `service.vision` |
| `vision.imlib` | `solar_os_native_vision_abi.h` | `service.imlib` |
| `inference` | `solar_os_native_inference_abi.h` | `service.inference` |
| `tensor.image` | `solar_os_native_inference_abi.h` | `service.inference` |

Each table has ABI version one and its own `struct_size`. The accompanying
`*_types_v1.h` data layouts and enum values are fixed for that ABI. New service
table members can be appended; incompatible data layouts require a new ABI.
Use the target's normal C ABI and enum sizing.

The host additionally exports the standard C helpers `memcpy`, `memset`,
`memmove`, `memcmp` and `strlen`, including compiler-generated uses. Allocate
dynamic module buffers through `memory.alloc`; they use PSRAM and must be
released with `memory.free`. Zero-size allocation and exhaustion return NULL.

## Images and video

`image.open` accepts a resolved storage path, such as `/sdcard/picture.jpg`.
`decode` accepts borrowed compressed bytes; `from_pixels` copies GRAY8,
little-endian RGB565 or RGB888 rows. All return one owned image reference.
`retain` adds a reference and `release` removes one. `pixels` borrows immutable
RGB888 rows, valid while an image reference remains held. `convert` writes a
crop/resize into caller-owned memory without modifying the source.

`media.video.create` creates a calling-task-owned session with optional
cancellation. `open` opens a local video stream ID with requested dimensions,
JPEG quality and timeout; it rejects non-video streams. `acquire` leases one
compressed JPEG and `frame` returns its borrowed bytes and metadata.
`release_frame` returns the lease. `snapshot` combines one-shot source opening
and capture; releasing that frame also closes its temporary source.

When `capabilities` includes `SOLAR_OS_NATIVE_MEDIA_RTSP`, use `rtsp_open`,
`rtsp_read` and `rtsp_ended`. Without that capability these operations return
`SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED`. RTSP receives JPEG video without audio
playback. `rtsp_read` can succeed with frame ID zero when no complete frame is
ready. Source/frame IDs belong to one session and are invalid after release.

Frame timestamps are capture time locally and arrival time for received RTSP.
Network views additionally include the original RTP timestamp and arrival
time. Decode through `image.decode`, then return the compressed lease before
processing the independent decoded image. Camera exclusivity and bounded
latest-frame RTSP behavior follow the existing media service.

`close` releases a source and any held frame. `destroy` closes all sources and
joins the RTSP worker before freeing the session. It does not release decoded
images, which have independent references. If a provider fails to close,
`destroy` returns an error and keeps the session valid for retry. Finish this
cleanup before returning from the module or its stop callback.

## Vision and models

`vision.qr.qrcodes` returns an owned result with payload bytes and original-image
corners. `vision.imlib.defaults` fills default options; `run` selects one of the
histogram/statistics, threshold/inversion, filtering, morphology, difference
or blob operations. Both use existing image references and leave inputs
unchanged. Both tables provide `result_free` for their own result type.

An imlib transform result owns its returned image. Call `image.retain` before
freeing the result if the image must remain usable. Analysis results and QR
payloads survive input image closure, but remain valid only until result release.

`inference.create` owns client cancellation and metadata state. `load` and
`load_bundle` return OS-owned resident handles; `find` and `list` discover them.
`info` borrows a client metadata snapshot until its next successful `info` call
or destruction. `run` accepts named dense tensors; `run_bundle` additionally
accepts image pixel views and applies declared adapters. Result buffers belong
to the host until `result_free` and survive subsequent runs or model unloading.

Client destruction does not unload models. Explicit `unload`/`unload_all`
release them. Retain a shared model while using it across other clients'
lifecycle controls; `release` drops that reference. Retained models report busy
on unload. `reset` and `set_mode` affect the shared resident instance.

`tensor.image.defaults`, `inspect` and `prepare` expose the same native crop,
resize, layout, normalization and quantization used by model bundles.
`inspect` reports the required size and inverse-map geometry. The module owns
the destination buffer passed to `prepare`; borrowed source pixels and tensor
descriptors must remain valid throughout the call.

## Errors, cancellation and lifetime

The new tables return `SOLAR_OS_NATIVE_OK` or the negative result constants in
`solar_os_native_service_abi.h`; they do not expose ESP-IDF error numbers.
Admission contention returns `SOLAR_OS_NATIVE_ERROR_INVALID_STATE`.
Cooperative cancellation and expired deadlines return
`SOLAR_OS_NATIVE_ERROR_TIMEOUT` after processing workers have joined.

Cancellation callbacks run synchronously on the calling task. Keep callback
code and user data alive until their client/session is destroyed. Serialize
calls on a client/session. Blocking capture, decode, vision and inference calls
belong outside native job tick callbacks.

A command-style module must release all images/results and destroy every
client/session before returning. A resident job must perform that cleanup and
join any module-owned worker before its stop callback returns. The native
runner unloads command modules immediately; resources are not automatically
collected. Models may remain resident because they contain no module callbacks.

## Minimal image client

```c
#include "solar_os_native_abi.h"
#include "solar_os_native_image_abi.h"

int main(int argc, char **argv)
{
    const solar_os_native_host_api_v1_t *host = solar_os_native_host_v1();
    if (!host || host->abi_version != 1 || host->struct_size < sizeof(*host) || !host->get_service) return 1;
    const solar_os_native_image_api_v1_t *image = host->get_service(
        SOLAR_OS_NATIVE_IMAGE_SERVICE, SOLAR_OS_NATIVE_IMAGE_ABI, sizeof(*image));
    if (!image || argc != 2) return 2;
    solar_os_raster_image_t *picture = NULL;
    int result = image->open(argv[1], &picture);
    if (!result) {
        solar_os_raster_image_pixels_t pixels;
        result = image->pixels(picture, &pixels);
        /* Use pixels.data, width, height and stride while picture is held. */
    }
    image->release(picture);
    return result ? 3 : 0;
}
```

`modules/vision-check` contains a standalone client of all six tables, including
model reuse and leased video decoding.

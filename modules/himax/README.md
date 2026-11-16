# Himax HX6538 processor

`processor-himax` is an optional native library for SSCMA model discovery and
camera-bound inference, model programming and slot selection. The expansion driver owns SPI and camera sampling;
this module owns programming policy, inference commands and result interpretation. It uses the
versioned `camera.sscma` host service and shares exclusive camera ownership with
Photo, camera captures and stream publishers.

The Watcher board profile selects `backend.processor-himax`, compiling this
same provider into its firmware. Firmware builders can omit that package from
the board's required packages and install the external library instead. Other
ESP32-S3 boards with a `himax-hx6538` expansion camera can use either form.
Built-in providers do not load or depend on an installed ELF. Installed
providers load on demand and retain a library lease until the pipeline closes.

## Use

```text
pipeline info himax
pipeline info himax camera0 models
pipeline start himax camera0 1
pipeline result 1
pipeline destroy 1
```

On firmware without the built-in provider, install `processor-himax` from a
signed module repository that supplies it before using these commands. Building
an ELF alone does not publish it to that repository.

The source must be the attached HX6538 camera's stream ID. Files, other camera
drivers and RTSP sources are unsupported by this provider. They can still use
the existing QR or ESP-DL processors.

Inference uses the Himax's currently configured model when `model=0` in the pipeline
configuration. A nonzero value selects that device slot, retaining the selection
after the pipeline closes. It sends `AT+INVOKE=1,0,0`, retains the event's JPEG alongside its
results, and performs no ESP32 pixel decoding. The module preserves SSCMA's
`boxes`, `classes`, `points`, `count` and `perf` fields. `coordinate_space` is
`sscma`; no unverified coordinate conversion or class labels are applied.
`perf` contains preprocessing, inference and postprocessing milliseconds.

Queries are `info`, `version`, `models`, `model`, `algorithms`, `identity`, `metadata`, and `slots`. Slot metadata
is reported as received: some factory firmware reports zero type/size values
even while inference works. No readiness decision is based on these fields.

Model management uses the provider's optional management callbacks. The generic
host owns dispatch and the `model-upload` background job, while this module
validates schema-two bundles and implements the SSCMA/WE2 SPI programmer:

```text
model upload himax camera0 /path/to/bundle.json 4 --replace
job status model-upload
model select himax camera0 4
model slots himax camera0
```

Upload protects slot 1 and the active slot, requires the reported four-slot
layout, and checks hashes, labels, model interfaces and firmware requirements
before writing. It checks page/CRC status, tests inference, restores the old
selection and atomically updates `/flash/.himax/<chip-id>.json`. `model slots
himax camera0` joins live selection with recorded slot identities. Missing records
remain unknown; removing a Zoo bundle does not remove this inventory. Uploads
require mounted, writable internal flash. No atomic rollback of flash contents
or full-image flash readback is claimed. See [the user manual](../../doc/manual/himax-models.md)
for cancellation, recovery, programming limits and the separate download step.
Threshold changes, firmware updates and ESP-DL resident handles are unsupported.

## Build

Build/configure an ESP32-S3 firmware SDK, then build the standalone library:

```sh
pio run -e seeed_sensecap_watcher
python3 modules/himax/build.py --sdk-build-dir .pio/build/seeed_sensecap_watcher
```

The output is `.pio/modules/himax/processor-himax.library.elf` with an import,
size and SHA-256 report in `build.json`. The builder checks the loader's section
and relocation subset. Its only firmware imports are the native library host
and standard memory/string-length helpers.

`himax-check.app.elf` is a development probe. With the library installed under
`modules/libraries/processor-himax.elf`, run it using `load /path/himax-check.app.elf`.
It verifies three load/query/capture/close/unload cycles and prints
`HIMAX_LIBRARY_OK cycles=3 captures=3`. It exports no camera images. This also
checks the external provider on firmware containing the built-in provider.

The provider ABI is `include/solar_os_processor_provider_abi.h`. Consumers must
hold a native-library lease through every call and client close. Copy borrowed
frames before `release_frame`; JSON uses host memory and can outlive the module.
The module has no global clients, background tasks or persistent allocations.
Management also uses the host's `storage.files` and strict `json` tables. Close
file leases and JSON roots before releasing the provider lease. Older providers
retain the capture ABI prefix; callers check size and capability bits before
accessing appended management methods.

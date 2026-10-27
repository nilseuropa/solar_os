# ESP-DL inference library

The `espdl` resident library supplies the ESP32-S3 inference engine for firmware
that enables `service.inference` without `backend.inference-espdl`. It uses
ESP-DL 3.3.13 with the configured SDK operator registry, including single, auto,
and dual-core execution. It accepts validated, unencrypted, single-model
EDL1/EDL2 buffers. File access, bundle validation, input adapters, model admission,
and scripting APIs belong to the firmware frontend.

## Usage

Mount storage and install from the signed catalog for your firmware version:

```text
pkg install espdl
model backend
model load /sdcard/model.espdl
```

The library loads when the frontend first loads a model. Each model holds a
lease until it closes and its workers finish. After closing all models,
`pkg unload espdl` releases library code and runtime allocations without removing
the installed file. `pkg remove espdl` also removes the file. Built-in firmware
always uses its built-in engine. See [Model inference](../../doc/manual/inference.md)
for Python, Lua, native clients, bundles, and execution modes.

## Build and catalog

Use the project's ESP-IDF 5.5 toolchain with GCC 14.2.0 and PlatformIO installed:

```sh
python3 modules/espdl/build.py
```

Outputs are `.pio/modules/espdl/espdl.library.elf`, arithmetic and vision probes,
a debug ELF, and `build.json` with artifact hashes. The builder configures a
separate SDK under `.pio/espdl-sdk/build/<environment>` using a generated copy
of `full` with `inference_backend` enabled, then builds the ESP-DL and schema
archives. It creates the compile commands automatically and refreshes the SDK
on later runs. It does not compile or flash a firmware image or edit the
checked-in flavor files.

`--environment` selects an ESP32-S3 PlatformIO environment; `--output` selects a
folder. `SOLAROS_PIO` can select the PlatformIO executable. To reuse an already
prepared SDK instead, pass `--sdk-build-dir /path/to/build/<environment>`; it must
contain both archives and ESP-DL compile commands from a build with the backend
enabled. The standard `full` flavor uses downloadable inference and does not
supply these prerequisites by itself. CMake's `typed_elf` target wraps
the same builder. CID's module deployment pipeline accepts this module's
`module.toml`, validates imports and ELF structure, and signs the catalog.
`SOLAROS_ESPDL_ENVIRONMENT` selects the SDK environment during deployment.

The builder makes private archive copies, replaces only the worker dispatcher,
binds C++ symbols within the library, and verifies the final stripped ELF's
imports and mapped relocations. Its only imports are the library host and
`memcpy`, `memmove`, `memset`, `memcmp`, and `strlen`. The firmware ownership
patch must be present in the SDK archives; the builder rejects unpatched sources.

The private runtime manages constructors, finalizers, exception frames, tracked
allocations, and exception TLS. Dual workers use versioned host synchronization
and joined internal stacks. Exceptions stay inside the provider; copied output
buffers belong to the host and may outlive the model and library. Unavailable
file/partition/crypto paths cannot be used by this buffer-only interface.
Toolchain upgrades require review of exception-frame relocation and runtime layout.

The frontend transfers its aligned model-file allocation to providers that
support `load_owned_buffer`, avoiding a second file-sized allocation during load.

Model weights and tensor descriptors remain loaded between calls. Activation
workspace is acquired for each inference and released after its outputs are
copied, including cancellation and error paths. Image decoding and other idle
work can use that space between calls. Loaded-model memory estimates exclude
the released workspace; execution still needs a contiguous workspace block.
The original borrowed-buffer operation still copies its input. Both paths free
the owned model source when the model closes, including failed construction.

## Hardware checks

The arithmetic probe checks scalar int8, float32, SIMD int8, all execution modes,
cancellation, oversized workspace rejection, leases, copied-output lifetime,
and 15 complete library/model cycles:

```sh
python3 tests/hil/espdl_library.py --telnet DEVICE_ADDRESS \
  --artifacts .pio/modules/espdl --output .pio/espdl-arithmetic-check
```

Upload mode requires FTP, refuses an existing `espdl` install, uses unsigned
maintainer ELFs, and removes only its own uploads. `--installed --probe PATH`
uses staged files and leaves them in place.

For real-model parity, supply compatible vision models explicitly:

```sh
python3 tests/hil/inference_backends.py --telnet DEVICE_ADDRESS \
  --probe .pio/modules/espdl/espdl-vision-check.app.elf \
  --model /path/to/face-model.espdl --model /path/to/classifier.espdl \
  --output .pio/espdl-baseline --keep-fixtures
```

After changing to firmware without a built-in engine and installing the signed
library, repeat with `--installed .pio/espdl-baseline/fixtures.json` and
`--expected .pio/espdl-baseline/results.json`. The probe exercises typed inputs,
all execution modes, cancellation/recovery, output parity, and worker teardown.
Use `--cleanup --installed PATH` to remove retained fixtures. Firmware flashing
and catalog hosting are separate maintainer actions.

Measured inference timing includes cooperative layer delays. Compare equivalent
frontend workloads; these probes do not establish camera throughput or detector
accuracy on images. Model files, workspace, copied outputs and loaded code all
consume PSRAM; externalization saves firmware flash, not active inference RAM.

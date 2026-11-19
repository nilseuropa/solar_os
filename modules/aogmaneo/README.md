# AOgmaNeo resident library

`aogmaneo.library.elf` provides online learning through sparse predictive
hierarchies, reinforcement-learning actors, and image encoding/reconstruction.
It runs on ESP32-S3 with executable PSRAM and the native-module service.
The engine remains in the optional ELF. The `service.aogmaneo` firmware frontend
provides Python/Lua bindings and is selected by the Python and Lua groups on
supported boards. It registers no shell command.

The versioned C API is [solar_os_aogmaneo_abi.h](../../include/solar_os_aogmaneo_abi.h).
Native apps and jobs acquire library ID `aogmaneo`, provider ABI 1, and retain
their lease until every model has been destroyed. The table supports up to eight
hierarchy/image models in total. Shutdown refuses while any model is live.
All calls run synchronously on the caller's task, using a single execution
thread. Overlapping or reentrant calls return `INVALID_STATE`.

Hierarchies accept categorical columns with input-only, prediction or action
roles, recurrent layers, online learning, reward and imitation input, predicted
categories, activations and actor value estimates. The image encoder accepts
byte channels, returns categorical columns and reconstructs visible layers.
Each model has independent seeded random state. Learning parameters use the
pinned upstream defaults. ABI 1 does not expose model persistence.
Python and Lua expose `solaros.aogmaneo` with matching hierarchy, actor and
image operations. Each interpreter owns its model handles and closes them on
exit, including after script errors. Explicitly destroy models when finished.

Buffers and descriptors are copied into provider-owned storage. Outputs are
copied into caller buffers. No C++ object, borrowed output pointer, exception,
file, display or worker is exposed. An allocation or engine failure during a
step invalidates that model; destroy and recreate it. See the
[manual](../../doc/manual/aogmaneo.md) for dimensions, buffer order and usage.

## Build

```sh
python3 modules/aogmaneo/build.py
```

The builder configures an isolated PlatformIO SDK under `.pio/aogmaneo-sdk`
for `esp32_s3_devkitc1_n16r8`, without building or flashing firmware.
`--environment` selects another ESP32-S3 SDK. Reuse an existing configured SDK
with `--sdk-build-dir .pio/build/esp32_s3_devkitc1_n16r8`. `--output` selects
the artifact directory. CMake exposes `typed_elf` for module catalog tooling.

Outputs under `.pio/modules/aogmaneo/` are `aogmaneo.library.elf`,
`aogmaneo.debug.elf`, and `build.json` containing the upstream revision, SDK
directory, artifact size, SHA-256 and verified imports. The public C header is
embedded in the library ELF for the installed `cc` SDK. The private C++ runtime,
linker layout and exception-frame tools come from `modules/espdl`; the ESP-DL
library is not a runtime dependency. GCC 14.2.0 is required for this runtime.
The builder rejects unsupported imports and relocations.

Once the module is in a compatible signed catalog, install it with
`pkg install aogmaneo`. It is stored as `modules/libraries/aogmaneo.elf` in
the preferred volume. Acquisition loads the installed file without network
access. Unused libraries can be unloaded with `pkg unload aogmaneo`.

## Script examples

Run [Python](../../examples/python/aogmaneo.py) or
[Lua](../../examples/lua/aogmaneo.lua) after installing the library. Both
examples train a hierarchy and encode/reconstruct an image. Model/layer indexes
are zero-based in both languages; Lua list tables use their normal one-based
array positions. See the [manual](../../doc/manual/aogmaneo.md) for signatures,
descriptor fields and defaults.

## Native C example

Copy [examples/predict.c](examples/predict.c) and
[examples/cc.toml](examples/cc.toml) into the same device directory. With `cc`
and the library installed, run:

```text
cc build
load predict.elf
```

Upstream source, revision, local changes and licensing are recorded in
[vendor/README.solaros.md](vendor/README.solaros.md). AOgmaNeo uses
**CC BY-NC-SA 4.0**; commercial use requires an appropriate upstream license.

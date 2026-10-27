# Jet resident library

`jet.library.elf` supplies the Jet renderer behind the firmware's `service.jet`
frontend and the Python/Lua `solaros.jet` bindings. It is loaded on demand from
`modules/libraries/jet.elf`, with a lease for the entire scene lifetime. The
renderer is not compiled into the firmware and registers no shell command.

The C provider ABI is [solar_os_jet_abi.h](../../include/solar_os_jet_abi.h).
It provides a camera, directional and ambient lighting, cubes, indexed triangle
meshes, transforms, removal and synchronous cancellable rendering. The host owns
display claims, color/depth buffers and the shared presenter. RGB565 output is
already in LCD wire byte order; MONO1 dithering belongs to the host presenter.
Only one scene may be live, and calls must be serialized, because upstream still
has shared material/trigonometric state. Exceptions stay inside the provider.

The optional size-checked extension adds reusable materials, textures and palette
animation, mesh attributes, primitives, scene/camera controls, point lights,
sprites, picking, in-memory asset loading, shared mesh instances, particle pools,
lens flares, post-processing, tiled indexed textures, procedural shaders and
renderer controls. Python and Lua expose the same operations. Script shader
errors are contained during rendering and re-raised after the provider returns.
Older providers retain the base scene API; scripts can query
`capabilities()` before using extensions. Resources reject removal while in use.

See the [Jet manual](../../doc/manual/jet.md) for script APIs and limitations.
Examples are [Python](../../examples/python/jet.py) and
[Lua](../../examples/lua/jet.lua). Install the library from a compatible signed
catalog with `pkg install jet`. The firmware does not download it while opening
a scene. Close all scenes before `pkg unload jet` or `pkg remove jet`.

## Build

Use the SolarOS ESP32-S3 toolchain and PlatformIO:

```sh
python3 modules/jet/build.py
```

Outputs are `.pio/modules/jet/jet.library.elf`, `jet.debug.elf`, and `build.json`
with the artifact hash, imports and SDK environment. The builder configures a
private SDK under `.pio/jet-sdk`, without building or flashing firmware.
`--environment` chooses an ESP32-S3 PlatformIO environment; `--sdk-build-dir`
can reuse a configured SDK containing `compile_commands.json`. `--output`
selects the artifact folder. CMake's `typed_elf` target wraps this builder for
the module deployment pipeline. `module.toml` declares a library module.

The build reuses the private C++ runtime, exception relocation checker and
linker layout from `modules/espdl`; the installed ESP-DL library is not required.
All runtime symbols are localized in Jet's ELF. Toolchain GCC 14.2.0 is pinned
for the exception-frame layout. Allocation, synchronization, time and libc
operations use versioned host services. The runtime clears exception TLS after
each provider call and releases constructors, exception frames and tracked
allocations on unload. The final ELF imports only the library host and five
standard memory/string helpers; the builder rejects unsupported imports or
relocations. Jet's DRAM-attributed constant tables are included in a mapped
data section.

Upstream provenance and local renderer changes are recorded in
[main/vendor/README.solaros.md](main/vendor/README.solaros.md).

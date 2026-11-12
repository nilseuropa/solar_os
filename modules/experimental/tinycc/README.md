# TinyCC Xtensa prototype

This prototype compiles C **on ESP32-S3** into native modules that use the
published SolarOS host and library ABIs. The `tinycc` native library exposes
a bounded in-memory compilation API; the `cc` prototype client tests it and
can emit the resulting ELF as hex. A Linux-hosted driver is also available.
File-based shell compilation and normal SDK/package installation remain
the next integration stage.

TinyCC is pinned by commit and archive checksum in `upstream.json`. Upstream
does not provide an Xtensa backend; `xtensa-gen.c` is a new, restricted backend
for the ESP32-S3 windowed calling convention. The overlay and build steps are
local and reproducible; no upstream checkout or firmware source is modified.
TinyCC and these backend changes use LGPL-2.0-or-later; the fetched source
contains upstream licensing and copyright notices.

## Implemented path

1. TinyCC preprocesses and parses one C translation unit, generating an
   Xtensa ELF32 relocatable object with `R_XTENSA_32` word relocations.
2. The portable C writer in `module.c` lays out `.text`, `.rodata`, `.data`
   and `.bss`, preserves symbol definitions, and emits a shared ELF with
   `R_XTENSA_RELATIVE` internal pointers and `R_XTENSA_JMP_SLOT` imports.
   Compiling user programs does not require GNU `as` or `ld`.
3. SolarOS resolves the versioned host symbol. Programs obtain services and
   installed library tables through that ABI, acquire a lease, call the
   provider and release the lease. Library headers still have to be provided
   to the compiler; dynamic tables do not supply C declarations.

The backend handles scalar integers up to 32 bits, pointers, byte/halfword
access, local arrays, global data, control flow, recursion, function pointers,
static tables, and register/stack arguments. It checks jump and frame ranges.
Floating-point operations, 64-bit runtime arithmetic, aggregate arguments and
returns, variadic definitions, variable-length arrays, computed goto and
inline assembly are unsupported. This is an initial ABI-oriented subset,
not a claim of general C conformance or support for every ESP32 variant.
There is no optimizing pass, general object/archive linker or complete libc.

## Build and check

Run from the firmware repository. Python 3.12+, a host C compiler, and the
installed PlatformIO ESP32-S3 toolchain are used by these commands:

```sh
python3 modules/experimental/tinycc/build.py --s3-check --gcc-provider
python3 modules/experimental/tinycc/check.py
```

`build.py` verifies the upstream archive before applying the backend overlay.
Pass `--archive /path/to/tinycc.tar.gz` to reuse a verified local download.
`--s3-check` cross-compiles the compiler core and module writer and checks
constant storage on a 32-bit host. `--gcc-provider` builds the unchanged
`hello-library` with GCC for interoperability checks.

`check.py` compiles the two probes and the unchanged `hello-library` and
`library-user` examples. It runs the production SolarOS ELF validator and
the existing relocation/import verifier on every result, and checks that
seven unsupported or malformed source cases produce errors. Results are
in `.pio/tinycc/checks.json`.

For another source file, use the include directory under the pinned source:

```sh
.pio/tinycc/solar-tcc example.c .pio/tinycc/example.o \
  modules/experimental/tinycc/sdk \
  .pio/tinycc/source/tinycc-aade7bafb59641660bc9a0e21ad0a9a4a1ed346e/include \
  include modules/examples/hello-library/main
.pio/tinycc/module .pio/tinycc/example.o .pio/tinycc/example.elf main
.pio/tinycc/elf-check .pio/tinycc/example.elf
```

The SDK `stdint.h` supplies target declarations; using the Linux system
headers for an ESP32 translation unit would give incorrect types/layouts.
GNU tools do not accept TinyCC's Xtensa `SHT_REL` intermediate objects;
inspect the final `SHT_RELA` ELF instead.

## Execution checks

### On-device compiler library

Build the library and client with the installed ESP32-S3 GCC toolchain:

```sh
python3 modules/experimental/tinycc/native_build.py
python3 modules/experimental/tinycc/qemu.py --native --qemu /path/to/qemu-system-xtensa
python3 modules/experimental/tinycc/native_hil.py --telnet go
```

Artifacts are `.pio/tinycc/native/tinycc.library.elf` and `cc.app.elf`.
`native/build.json` records their hashes, imports, SDK hashes, compiler version
and source hashes. The manifests are in `native-library` and `native-client`;
these experimental modules are not added to the published package catalog.

The library uses the existing `memory`, `thread`, `time` and `libc` services.
It imports no private firmware symbols. Each request runs on a joined 16 KiB
worker, owns a tracked private heap and reads headers from a virtual filesystem.
The SDK embeds the published native ABI headers and basic C type headers.
Clients can supply additional module-library headers as named memory buffers.
The compiler neither opens device files nor writes storage.

| Resource | Limit |
| --- | --- |
| Source | 64 KiB, NUL-terminated |
| Additional headers | 32 files, 32 KiB each, 256 KiB total |
| Private compile heap | Default/max 2 MiB, configurable down to 64 KiB |
| Output ELF | 2 MiB maximum, also limited by caller capacity |
| Deadline | Cooperative, default 2000 ms, maximum 5000 ms |
| Stack | Function-entry guard at 12,000 bytes on the 16 KiB worker |

The private heap limit includes compiler allocations, the intermediate object
and module-writer allocations. Resident code/data, the worker and caller-owned
input/output buffers are separate; host allocator overhead is additional.
The library serializes compilation and
rejects concurrent requests. Errors, resource-limit failures and cancellation
free request allocations and restore private compiler globals before returning.
Output buffers belong to the caller and remain valid after releasing its lease.
The embedded runtime rejects floating literals and fixes `__DATE__`/`__TIME__`
to the Unix epoch; it defines `__SOLAROS__` and `__ESP32S3__`.

When the client is loaded, its modes are:

```text
cc --self-test
cc --demo-hex
cc --scalar-hex
cc --emit-hex SOURCE
```

`--demo-hex` compiles `examples/library_probe.c` on the device. The hardware
test retrieves that ELF as hex, validates it on the host, transfers the same
bytes back to the device, and executes them through SolarOS's native loader.
Compilation occurs entirely on the device. The host supplies the temporary
file transport because the file-based shell interface is not implemented yet.
The test refuses existing test paths, removes its libraries and files, and
does not change firmware or package catalogs. Results are under
`.pio/tinycc/native-hil`; emulator results are under `native-qemu`.

### Host-generated code

With the official Espressif Xtensa QEMU installed locally:

```sh
python3 modules/experimental/tinycc/qemu.py --qemu /path/to/qemu-system-xtensa
```

The isolated ESP-IDF harness uses the repository's ELF loader, a GCC host
table and mock library service. It runs the scalar probe, 100 fresh
load/call/release/free cycles against a GCC provider, then loads the
TinyCC-generated provider and unchanged job example. GCC calls the generated
provider, shutdown, start, tick and stop callbacks; the job acquires and
retains independent leases. Emulator logs and hashes go in
`.pio/tinycc/qemu`. This harness disables internal-memory protection to make
SRAM executable; it is separate from SolarOS firmware configuration.

See [Espressif's QEMU guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/tools/qemu.html)
for the emulator. The validated version was
`esp_develop_9.2.2_20260417`.

For a network-connected SolarOS ESP32-S3 device:

```sh
python3 modules/experimental/tinycc/hil.py --telnet go --runs 20
```

This test temporarily uploads the probes and the GCC-built `hello-library`
through a local HTTP server. It refuses pre-existing test paths, uses the
device's native loader and library service, unloads the provider after each
cycle, records memory readings and artifact hashes, and removes its files
in cleanup. It does not change firmware or package catalogs. Logs and the
report go in `.pio/tinycc/hil`.

## Validated boundary

On 2026-11-12, branch `feature/embcc`, firmware baseline
`0af0c18fb024f18809e28bde0916ccdc9f259312`:

- All four modules passed the production ELF validator and relocation checks.
- The emulator passed the scalar checks, 100 ABI cycles and generated
  provider/job callbacks.
- `go`, a LilyGO T-Deck Plus with ESP32-S3 and SolarOS 4.17.10 full, passed
  the scalar and real library probes plus 20 repeated cycles. The provider
  returned 42, leases permitted unloading after every cycle, and reported
  free memory matched before and after the repeated runs. Temporary files
  and the test provider were removed.
- Address/undefined-behavior sanitizers found no errors in the C module
  writer across 300 truncated or randomly mutated intermediate objects.
- The compiler core and module writer cross-compiled for ESP32-S3. The
  unlinked compiler object measured 121,879 bytes text, 1,180 data and
  83,903 BSS; the writer measured 3,183 text. The largest individual reported
  compiler stack frame was 2,096 bytes. These are object/frame measurements,
  not an installed compiler footprint, total stack bound or runtime budget.

## Next integration boundary

The on-device library was also validated on `go` with SolarOS 4.17.10 on
2026-11-12. Two self-test runs checked identical repeated output, syntax-error
recovery, heap/stack/output limits, cancellation, deadlines, extra headers and
successful compilation after failures. The example compiled in about 70 ms,
with a 577,287-byte peak private heap; reported free memory matched before
and after the repeated self-test. Its 1,448-byte generated ELF passed the
production validator and executed on the device, returning 42 through the
GCC-built example library. The device also compiled and executed the scalar
probe, covering arithmetic, branches, arrays, stack arguments, recursion and
function pointers with a 32-bit compiler host. Both libraries unloaded and temporary files were
removed. The ESP32-S3 emulator independently compiled and executed the same
program and passed the recovery tests.

The next stage is a file-based `cc input.c -o output.elf` interface with shell
working-directory handling and safe storage replacement. SDK/header package
discovery, compiler diagnostics presentation, additional language support and
wider correctness tests remain work for that stage. The present proof covers
in-memory compilation and native library interoperability.

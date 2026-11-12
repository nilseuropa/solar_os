# TinyCC Xtensa prototype

This prototype compiles C on a Linux host into ESP32-S3 native modules that
use the published SolarOS host and library ABIs. The generated programs run
on SolarOS. The compiler is **not yet an on-device command or installable
module**: its file/runtime adapters and resource controls remain to be built.

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

To compile on the device, package the compiler with bounded filesystem,
allocator and C-runtime adapters exposed through a versioned SolarOS service.
Set compile heap/stack/source/output limits and make errors and cancellation
release all compiler state. Then compile source on the actual device, write
the output through SolarOS's safe storage replacement API and execute the
result through the existing native loader. SDK/header packaging, diagnostics,
additional language support and wider correctness tests remain work for that
stage. Running generated code on the device does not establish that the
compiler itself has run there.

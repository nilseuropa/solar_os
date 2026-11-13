# TinyCC Xtensa prototype

This prototype compiles C **on ESP32-S3** into native modules that use the
published SolarOS host and library ABIs. The `tinycc` native library exposes
a bounded compilation API; the `cc` client reads source and headers from
device storage and writes validated native ELF files. A Linux-hosted driver
is also available. These modules remain an experimental installation.

TinyCC is pinned by commit and archive checksum in `upstream.json`. Upstream
does not provide an Xtensa backend; `xtensa-gen.c` is a new, restricted backend
for the ESP32-S3 windowed calling convention. The build verifies and extracts
the pinned archive under `.pio/tinycc/source`, then applies the local overlays
there.
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

## GCC comparison tests

Run the scalar32 comparison suite with the installed ESP32-S3 toolchain and
Espressif QEMU:

```sh
python3 modules/experimental/tinycc/differential.py --qemu /path/to/qemu-system-xtensa
```

The driver rebuilds both TinyCC hosts. Each source case is independently
compiled into native ELF by ESP32-S3 GCC at `-O0` and `-O2`, host-built TinyCC,
and the compiler library running inside the ESP32-S3 emulator. All four
versions execute through the same native loader. Both GCC references must
agree with each other and with each TinyCC program's return value, 32 output
words and the surrounding guard words. Every ELF passes
the production validator; compile/load failures, mismatches, missing cases,
crashes and emulator deadlines fail the command.

The default corpus has 22 curated cases and 16 seeded expression cases, each
executed for 128 input pairs through both TinyCC hosts: 9,728 comparisons.
It covers unsigned wrapping, bounded signed arithmetic, casts and narrow
loads/returns, booleans, comparisons, short-circuit side effects, loops and
switches, arrays/pointers, large stack offsets, structure members, bitfields,
object bytes, global relocations, recursion, register pressure, stack
arguments, GCC/TinyCC callbacks in both directions, alignment and imported
memory helpers. Target-specific signed casts/shifts and layouts are compared
against GCC for the same ESP32-S3 target. Generated expressions use unsigned
arithmetic, nonzero divisors and masked shift counts; cases avoid signed
overflow, invalid pointer arithmetic and unsequenced side effects.

For another deterministic corpus or a single regression case:

```sh
python3 modules/experimental/tinycc/differential.py --seed 0x12345678 \
  --random-cases 32 --vectors 256 --qemu /path/to/qemu-system-xtensa
python3 modules/experimental/tinycc/differential.py --case alignment \
  --qemu /path/to/qemu-system-xtensa
```

`--case unsigned_arithmetic --inject-mismatch` is a negative control: it
changes one output word in the host TinyCC program while leaving its return
value and the references unchanged. The command must exit unsuccessfully.
This checks that comparison failures cannot produce a passing report.

Sources, host/reference ELFs, compiler and test-source hashes, GCC flags and
successful results are under `.pio/tinycc/differential`. Each emulator run
archives its manifest, inputs, ELFs and console under `differential/runs`.
Failures identify the case, compiler host, input pair and differing words;
there is no passing report for a failed run. Current emulator output is under
`differential-qemu`. This is a growing regression corpus, not a C conformance
certification or proof for arbitrary programs.

The portable writer also has a host sanitizer regression, independent of the
downloaded compiler/toolchain:

```sh
python3 -m unittest discover -s tests -p test_tinycc_module_writer.py
```

It checks preservation of requested section alignment, including merged
sections, and rejection of unsupported alignments. The execution corpus's
`alignment` case additionally verifies alignment after native loading.

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
python3 modules/experimental/tinycc/file_hil.py --telnet go
```

Artifacts are `.pio/tinycc/native/tinycc.library.elf` and `cc.app.elf`.
`native/build.json` records their hashes, imports, SDK hashes, compiler version
and source hashes. The manifests are in `native-library` and `native-client`;
these experimental modules are not added to the published package catalog.

The library uses the existing `memory`, `thread`, `time` and `libc` services.
It imports no private firmware symbols. Each request runs on a joined 16 KiB
worker, owns a tracked private heap and reads headers from a virtual filesystem.
The SDK embeds the published native ABI headers and basic C type headers.
Clients can supply additional module-library headers as named memory buffers
or through a bounded header-reader callback. The `cc` client uses the shell's
`storage` service to resolve paths, read files and replace the output after
compilation and validation. The library itself has no filesystem imports.

### Compile files on SolarOS

The file command requires firmware with
`include/solar_os_native_storage_abi.h` support. Install `cc.app.elf` as
`/modules/apps/cc.elf` and `tinycc.library.elf` as
`/modules/libraries/tinycc.elf`. Paths use SolarOS's preferred storage volume.
For the example library probe, also install the GCC-built
`.pio/tinycc/hello-library.library.elf` as
`/modules/libraries/hello-library.elf`.

```text
cd /dev/cc-examples
cc hello.c -o hello.elf
load hello.elf
cc headers.c -I include -o headers.elf
load headers.elf
```

The examples print `TinyCC: native library returned 42` after acquiring,
calling and releasing the installed library. Users can edit `hello.c` with
the device's editor and compile it again. `cc input.c` defaults to `a.elf`.
Use `-I directory` or `-Idirectory` for additional library declarations;
quoted includes also search beside the source/header containing them.
The built-in SDK supplies the SolarOS native ABI headers and basic C types.
An installed module library supplies executable code; its C header must be
available to the compiler separately. The prototype does not discover or
install header packages automatically.

Paths are relative to the calling shell, including when invoking `cc` by
its installed name. One invocation compiles one translation unit, with at
most eight include directories. Diagnostics include source filenames and
line numbers. Unsupported kernel imports are rejected. The client validates
the completed ELF with the production SolarOS validator before writing it.
Errors and cooperative cancellation leave an existing output intact; output
replacement uses synced staging plus backup/restore on filesystem errors.
It reserves `.native.tmp` and `.native.bak` siblings and does not promise
power-loss atomicity. Ctrl+C or Ctrl+] requests cancellation.

`file_hil.py` requires the test paths and module names to be unused. It
installs the compiler, provider and examples, compiles and executes the
programs entirely on the device, checks header paths and error recovery,
compares output hashes, and leaves the useful files installed. It does not
update firmware. Its report and retrieved/validated ELFs are under
`.pio/tinycc/file-hil`.

| Resource | Limit |
| --- | --- |
| Source | 64 KiB, NUL-terminated |
| Additional headers | 32 files, 32 KiB each, 256 KiB total |
| Additional include directories | 8 |
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

The client also retains diagnostic modes:

```text
cc --self-test
cc --demo-hex
cc --scalar-hex
cc --emit-hex SOURCE
```

`--demo-hex` compiles `examples/library_probe.c` on the device. The hardware
test retrieves that ELF as hex, validates it on the host, transfers the same
bytes back to the device, and executes them through SolarOS's native loader.
Compilation occurs entirely on the device. This earlier in-memory harness
uses host transport for the returned ELF and can also run on firmware without
the native storage service.
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

## In-memory integration evidence

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

SDK/header package discovery, additional language support and wider
correctness tests remain future work. File compilation uses the same bounded
compiler API and native library interoperability path. The request extension
is append-only: callers using the original request prefix remain supported.

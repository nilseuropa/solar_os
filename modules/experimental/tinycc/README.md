# TinyCC Xtensa prototype

The installable packages are declared in `modules/cc` and `modules/tinycc`.
On compatible firmware, `pkg install cc` installs both through the signed
catalog. `pkg install jet` also supplies the Jet C headers. See
[`examples/jet-cube`](examples/jet-cube) for an editable native graphics app.

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

1. TinyCC preprocesses and parses up to eight separate C translation units, generating an
   Xtensa ELF32 relocatable object with `R_XTENSA_32` word relocations.
2. The portable C writer in `module.c` lays out `.text`, `.rodata`, `.data`
   and `.bss`, preserves symbol definitions, and emits a shared ELF with
   `R_XTENSA_RELATIVE` internal pointers and `R_XTENSA_JMP_SLOT` imports.
   Compiling user programs does not require GNU `as` or `ld`.
3. SolarOS resolves the versioned host symbol. Programs obtain services and
   installed library tables through that ABI, acquire a lease, call the
   provider and release the lease. Library headers still have to be provided
   to the compiler; packaged SDK headers provide these declarations.

The backend handles signed and unsigned integers through 64 bits, `float`,
`double`, pointers, byte/halfword access, local arrays, global data, control
flow, recursion, function pointers, static tables, and register/stack arguments.
It implements aligned two-word arguments and returns, by-value structures
(including register and hidden-pointer returns), and variadic definitions with
`va_start`, `va_arg`, `va_copy` and `va_end`. These calling conventions support
published SolarOS services and GCC-built module libraries. `long double` has
the target's 64-bit `double` representation. Jump and frame ranges are checked.

The compiler build packages target-GCC arithmetic and conversion helpers into
a closed private bundle. Generated programs that reference these helpers
receive the bundle with internal relocations; integer-only programs that do
not need helpers omit it. This adds roughly 10 KiB to helper-using programs;
the prototype does not yet select individual functions. No additional runtime
library installation or GNU tools are needed on the device. The bundled GCC
runtime retains its upstream licensing and runtime exception.

Variable-length arrays, computed goto and inline assembly remain unsupported.
There is no optimizing pass, general object/archive linker or complete libc.
Support is experimental and targets ESP32-S3's windowed ABI; this is not a C
conformance certification or support for other ESP32 variants.

## C SDK and projects

Compile separate source files together; each keeps its own macros, include
guards, static symbols and diagnostics. The total source limit remains 64 KiB:

```text
cc main.c helpers.c -o project.elf
load project.elf
```

Installed libraries may carry their C headers in a non-allocated
`.solaros.sdk` ELF section. A source can include
`<hello-library/hello_library_api.h>` without a header copy or `-I` flag. The
client resolves this namespace from the installed library file. Code and
headers share the package's artifact hash, installation and removal. See
[native libraries](../../../doc/manual/native-library.md#c-headers-in-library-packages)
for author declarations and the CMake packaging helper. Local quoted headers
and explicit `-I` directories remain available.

The SDK supplies `stdint.h`, `stddef.h`, `stdbool.h`, `stdarg.h` and these
documented subsets of standard C headers:

| Header | Supported operations |
| --- | --- |
| `string.h` | Memory copy/move/set/compare/search, string length/compare/copy/concatenation/search |
| `stdlib.h` | `malloc`, `calloc`, `realloc`, `free`, `abs`, `labs`, `atoi`, `atof` |
| `stdio.h` | `printf`/`fprintf`/`snprintf` and their `va_list` forms, `puts`/`fputs`, character I/O on files, `fopen`/`fclose`/`fflush`, `fread`/`fwrite`, `fseek`/`ftell`/`rewind`, EOF/error indicators |
| `math.h` | `floor`, `fabs`, `ldexp`, `expf`, `floorf`, `fmodf`, `logf`, `powf`, `sqrtf`, `fabsf`, infinity/NaN constants and classification |

### Browse and export headers

```
cc sdk list
cc sdk show stdio.h
cc sdk show jet/solar_os_jet_abi.h
cc sdk export /dev/sdk
```

Listing shows provider package versions and byte sizes. Built-in headers are
borrowed through the compiler's append-only API table; installed-library
headers and versions are read from their `.solaros.sdk` metadata without
activating those providers. Export preserves namespaces, validates the complete
inventory and header text before writing, and requires a new destination
directory. Existing directories are refused. Cancellation and ordinary write
failures clean up the files created by that export; cleanup errors identify
the remaining destination. The browser supports 128 built-in headers and up to
16 installed library SDKs, with the library SDK format's existing text limits.

The compiler API appends `sdk_version`, `sdk_count` and `sdk_headers`; their
read-only pointers remain valid while the provider lease is held. Earlier
compile clients continue to use `SOLAR_OS_CC_API_BASE_SIZE`. The scoped storage
API appends directory enumeration, directory creation and nonrecursive removal;
its original file-operation prefix remains available through
`SOLAR_OS_NATIVE_STORAGE_BASE_SIZE`.

When a program uses supported C functions, the compiler links one private C
runtime into its output. It calls published SolarOS services and imports no
private firmware symbols. All translation units share its streams and globals.
`stdout` and `stderr` write to the invoking shell. File I/O uses its scoped
storage service; modes are `r`, `rb`, `w`, and `wb`. Up to 16 files may be open.
Writes are buffered in memory with a 2 MiB file limit and use the storage
replacement operation at flush/close. Formatted stream output is bounded to
4,096 bytes per call; `snprintf` uses the caller's capacity. Returning from a
standard `main` closes and flushes remaining streams. Other entry points and
resident-library callbacks must close their files explicitly.

Allocation uses SolarOS memory services. Free allocations with their matching
allocator: SDK `malloc`/`calloc`/`realloc` pointers go to SDK `free`. Programs
must release their allocations before unloading. This subset does not provide
`stdin`, `exit`, `atexit`, `errno`, locale, POSIX descriptors or a complete math
library. The private runtime and required arithmetic bundle are currently
linked as whole units, with a corresponding output-size cost.

The `examples/project` directory contains a two-file application using an
installed library, standard formatting/math and file I/O. After `file_hil.py`
has installed matching compiler/provider artifacts, validate it on a device:

```sh
python3 modules/experimental/tinycc/sdk_hil.py --telnet DEVICE_IP
```

This leaves `/dev/cc-sdk-project` with editable examples. It checks automatic
header discovery, separate translation units, the C runtime, file operations,
automatic close on return, failed-build output preservation, SDK removal and
malformed metadata, recovery, and repeated builds without managed-memory loss.
Sources, artifact hashes and the console are under `.pio/tinycc/sdk-hil`.

## GCC comparison tests

Run the code-generation and ABI comparison suite with the installed ESP32-S3 toolchain and
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

The default corpus has 40 curated cases and 16 seeded expression cases, each
executed for 128 input pairs through both TinyCC hosts: 14,336 comparisons.
It covers unsigned wrapping, bounded signed arithmetic, casts and narrow
loads/returns, booleans, comparisons, short-circuit side effects, loops and
switches, arrays/pointers, large stack offsets, structure members, bitfields,
object bytes, global relocations, recursion, register pressure, stack
arguments, GCC/TinyCC callbacks in both directions, alignment and imported
memory helpers. Additional cases exercise 64-bit arithmetic and conversions,
decimal/hex floating literals, subnormals and special values, floating-point
services, structure arguments/returns at ABI boundaries, and variadic
definitions across register/stack boundaries. Callbacks exercise both
GCC-to-TinyCC and TinyCC-to-GCC calls, including forwarding `va_list` to
`libc.format`. Floating operations disable contraction in GCC references and
runtime helpers; float-to-integer inputs stay within defined conversion ranges.
Target-specific signed casts/shifts and layouts are compared
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

For a physical ESP32-S3 device with matching firmware and the compiler installed
by `file_hil.py`, run:

```sh
python3 modules/experimental/tinycc/differential_hil.py --telnet DEVICE_IP
```

This builds a temporary GCC library containing both reference versions and the
ABI callbacks. It transfers and hashes each C source, compiles it through `cc`
on the device, loads the generated ELF and compares all input vectors with both
GCC references on the same hardware. The default is 56 cases and 128 input pairs:
7,168 comparisons. It checks output guard words, library leases, unloading and
managed free memory. Compiler, provider, source and generated ELF hashes are
recorded under `.pio/tinycc/differential-hil`. The test requires unused
`/dev/cc-differential` and `tinycc-diff-check` paths and removes its temporary
provider and device files afterward; the installed compiler and file examples
remain available. Run hardware suites sequentially on the network shell.

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
four unsupported or malformed source cases produce errors. Results are
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

The `cc` client also supports declarative projects with `cc build` or
`cc build path/to/cc.toml`. The bounded TOML project lists sources, include
directories, an output and exact installed SDK dependencies. Paths are relative
to the project file; dependencies are checked without executing providers.
Builds recompile all sources and create output directories only after successful
compilation and ELF validation. See the [compiler guide](../../cc/README.md) and
[complete project example](examples/build-project) for the format and commands.
Project files are capped at 8 KiB and eight dependencies; compiler source,
include, heap, output and deadline limits still apply. Project parsing and file
path workspaces use request-owned heap storage.

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
An installed module library can supply both executable code and namespaced
headers in its ELF. Older libraries without SDK metadata still need declarations
through local headers, `-I`, or the compilation API's supplied header buffers.

Paths are relative to the calling shell, including when invoking `cc` by
its installed name. One invocation compiles up to eight translation units, with at
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
| Source | Up to 8 NUL-terminated files, 64 KiB total |
| Additional headers | 32 files, 32 KiB each, 256 KiB total |
| Additional include directories | 8 |
| Private compile heap | Default/max 2 MiB, configurable down to 64 KiB |
| Output ELF | 2 MiB maximum, also limited by caller capacity |
| Deadline | Cooperative: API default 2000 ms, file command 20000 ms, maximum 20000 ms |
| Stack | Function-entry guard at 12,000 bytes on the 16 KiB worker |

The private heap limit includes compiler allocations, the intermediate object
and module-writer allocations. Resident code/data, the worker and caller-owned
input/output buffers are separate; host allocator overhead is additional.
The library serializes compilation and
rejects concurrent requests. Errors, resource-limit failures and cancellation
free request allocations and restore private compiler globals before returning.
Output buffers belong to the caller and remain valid after releasing its lease.
Floating literal parsing uses the `libc` service. Decimal double literals need
the appended `parse_double` method; hexadecimal floating literals need
`scale_double`. Older firmware continues to support integer compilation and
reports a missing method before using either extension. Update firmware to
compile the full floating-point corpus. The emulator checks old table sizes,
missing method pointers, integer compilation and recovery after parser errors.
The runtime fixes `__DATE__`/`__TIME__` to the Unix epoch and defines
`__SOLAROS__` and `__ESP32S3__`.

The compile request appends a translation-unit array; both earlier request
prefixes retain their behavior. The API table appends `request_size` for
capability checks. Install matching compiler and client packages.
For the Linux-hosted low-level driver, standard C use prints the selected
`solar_os_c_main` entry; pass that name to the module writer to include stream
cleanup when `main` returns.

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

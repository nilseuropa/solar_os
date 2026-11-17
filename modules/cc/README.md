# C compiler

`cc` compiles C source files on ESP32-S3 devices with PSRAM into native SolarOS
applications. Install it with `pkg install cc`; its matching `tinycc` library is
installed automatically.

Firmware builders can instead enable the `cc` group in a custom flavor for an
ESP32-S3 board with PSRAM. It links this command and the TinyCC provider into
the image using the same sources and ABI. This form requires no installed
compiler packages. Standard flavors keep it disabled. An installed `cc` command
can also acquire the built-in `tinycc` provider through normal library leases.

```
cc main.c helpers.c -o program.elf
load program.elf
```

For a repeatable project build, put `cc.toml` beside the source directory:

```toml
sources = ["src/main.c", "src/helpers.c"]
include_dirs = ["include"]
output = "build/demo.elf"
dependencies = [{ id = "hello-library", version = "1.1.0" }]
```

```
cc build
load build/demo.elf
```

`cc build` reads `cc.toml` in the current directory. `cc build /dev/demo/cc.toml`
selects another project. Relative source, include and output paths are based on
the project file's directory, independent of the calling shell's directory;
absolute paths are also accepted. `sources` is required; `include_dirs` and
`dependencies` default to empty arrays, and `output` defaults to `a.elf` beside
the project file. The [project example](../experimental/tinycc/examples/build-project)
contains two source files and a local header.

Each dependency requires an installed library with SDK metadata matching its
exact version. Checks read the library ELF without executing the provider.
Missing libraries report a `pkg install ID` command; a version mismatch reports
both versions. The repository must provide the required version. Build settings
do not acquire runtime library leases; the application still uses the SolarOS
library API. The compiler does not download packages during a build.

The project format is a bounded TOML subset: the four root keys above, single
or double quoted strings, multiline arrays, comments and trailing array commas.
Double quoted strings support escaped quotes and backslashes. Inline dependency
tables contain `id` and `version` on one line. Other tables, multiline strings,
Unicode escapes, unknown keys and duplicate keys are rejected with a line
number. Project files are limited to 8 KiB, with at most eight source files,
eight include directories and eight dependencies. Paths fit in 160 bytes
including the terminator. Source lists contain explicit paths, with no wildcard
expansion or shell recipes.

Every build recompiles all listed sources. After successful compilation and ELF
validation, it creates missing output directories and replaces the executable
through the existing recoverable writer. Failed compilation, dependency checks
and cooperative cancellation preserve an existing executable. The output must
differ from the project and source files, including case variants on FAT.

Use `-I directory` for local headers. SolarOS ABI headers and a bounded standard
C library are included. Installed libraries can supply namespaced headers in
their ELF packages: Jet provides `<jet/solar_os_jet_abi.h>`.

Browse the available SDK directly on the device:

```
cc sdk list
cc sdk show jet/solar_os_jet_abi.h
cc sdk export /dev/sdk
edit /dev/sdk/jet/solar_os_jet_abi.h
```

`list` shows header names, provider package versions and sizes. `show` prints
the selected header. `export` requires a new destination directory, creates its
parents and preserves the include namespaces. Use another destination for a
fresh export after updating packages. Existing directories and edited headers
are preserved. Library SDK metadata is read directly from installed files.

The [Jet cube example](../experimental/tinycc/examples/jet-cube) builds a native
foreground app with input, suspend/resume and clean exit. The compiler accepts
up to eight source files totaling 64 KiB, eight include directories, a 2 MiB
private compilation heap and a 20-second cooperative deadline. It validates
the generated ELF before replacing the output file. This is a bounded C SDK;
it does not provide full libc, POSIX, optimization or a general object/archive
linker. See the [compiler reference](../experimental/tinycc/README.md) for the
supported language and runtime operations.

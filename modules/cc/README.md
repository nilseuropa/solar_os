# C compiler

`cc` compiles C source files on ESP32-S3 devices with PSRAM into native SolarOS
applications. Install it with `pkg install cc`; its matching `tinycc` library is
installed automatically.

```
cc main.c helpers.c -o program.elf
load program.elf
```

Use `-I directory` for local headers. SolarOS ABI headers and a bounded standard
C library are included. Installed libraries can supply namespaced headers in
their ELF packages: Jet provides `<jet/solar_os_jet_abi.h>`.

The [Jet cube example](../experimental/tinycc/examples/jet-cube) builds a native
foreground app with input, suspend/resume and clean exit. The compiler accepts
up to eight source files totaling 64 KiB, eight include directories, a 2 MiB
private compilation heap and a 20-second cooperative deadline. It validates
the generated ELF before replacing the output file. This is a bounded C SDK;
it does not provide full libc, POSIX, optimization or a general object/archive
linker. See the [compiler reference](../experimental/tinycc/README.md) for the
supported language and runtime operations.

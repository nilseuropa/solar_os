+++
id = "c"
title = "C development"
section = "api"
summary = "Compile native C programs, build projects, and browse SDK headers"
aliases = ["c.api", "c-compiler"]
keywords = "c cc tinycc native compiler programming projects sdk headers"
packages_any = ["service_native_modules"]
agent_reference_sections = true
+++
# C development on SolarOS

Write C source files on your device, compile them with `cc`, and run the
resulting native programs from the SolarOS shell. The compiler uses TinyCC
and targets ESP32-S3 boards with PSRAM.

## Get the compiler

The `full` flavor includes the compiler. On other firmware, enable the `cc`
group in a custom build or install the command and its matching library:

```text
pkg install cc
```

Check the device and available features, then display the compiler's usage:

```text
board
pkg system
cc --help
```

Bare `cc` also displays help. See [firmware packages and flavors](packages.md)
for build options, and [Native ELF modules](native-modules.md) for package requirements.

## Compile your first program

Use the [text editor](apps.md#edit) to create `hello.c` in the
current directory:

```text
edit hello.c
```

The editor automatically highlights C source (`.c`) and header (`.h`) files.

Save this program:

```c
#include <stdio.h>

int main(int argc, char **argv)
{
    puts("Hello from SolarOS C");
    return 0;
}
```

Return to the shell, compile it, and run the executable:

```text
cc hello.c -o hello.elf
load hello.elf
```

The compiler reports source errors or the completed output file. `load` runs
this synchronous program and releases its executable memory when it returns.
Native programs run in the firmware's address space and have access to its
hardware; run code you trust.

## Build a project

For repeatable builds, save a `cc.toml` beside `hello.c`:

```toml
sources = ["hello.c"]
output = "build/hello.elf"
```

Then run:

```text
cc build
load build/hello.elf
```

List additional source files in `sources`. Use `include_dirs` for local
headers and `dependencies` for exact versions of installed library SDKs.
`cc build /path/to/cc.toml` selects another project; its paths are relative
to the project file. Successful builds create missing output directories.
Compilation errors and cooperative cancellation preserve an existing output.
See [project files and library headers](native-library.md#c-headers-in-library-packages)
for the supported settings and dependency format.

## Explore the SDK

The compiler includes SolarOS native ABI headers and a bounded standard C
library. Installed native libraries can supply their public headers in the
same package as their code. Browse or export the available headers:

```text
cc sdk list
cc sdk show stdio.h
cc sdk export sdk-copy
```

Installed library SDKs include the supporting headers needed by their public
entry header. For example, after installing Robot, use
`cc sdk show robot/solar_os_robot_abi.h` to display its C interface, and
`#include <robot/solar_os_robot_abi.h>` also resolves the packaged model and
differential-drive headers. No manual header copy or `-I` option is needed.
See [library entry headers](native-library.md#c-headers-in-library-packages)
for Emotion, ESP-DL, Himax, and other native libraries.

Export requires a new destination directory. Use `-I directory` for local
headers. For multiple source files without a project file, use
`cc main.c helpers.c -I include -o program.elf`.

Native programs access SolarOS services and installed libraries through their
published C interfaces. Keep library leases until all calls, objects, and
workers using them have finished. Resumable graphical and terminal apps use
the foreground API to share display, input, audio, and sessions.

- [C compiler and SDK guide](native-library.md#compile-c-on-the-device)
- [Native library APIs and lifetime](native-library.md)
- [Native foreground apps](native-app.md)
- [Native media and inference services](native-media.md)

## Limits

The built-in and installable compiler use the same SDK and limits: up to
eight source files totalling 64 KiB, eight include directories, a 2 MiB private
compilation heap, and a 20-second cooperative deadline. The compiler validates
the generated ELF before replacing the output.

The SDK provides a subset of standard C library functions. Variable-length
arrays, inline assembly, optimization, and general object/archive linking are
unavailable. See the
[compiler reference](https://github.com/nilseuropa/solar_os/tree/main/modules/experimental/tinycc#c-sdk-and-projects)
for supported language features and library functions.

## Quick reference

On ESP32-S3 boards with PSRAM, the full flavor includes `cc`. Other builds can
select the `cc` firmware group or install it with `pkg install cc`. Inspect
`pkg system` for compiled packages. `cc` and `cc --help` show usage. Compile with
`cc hello.c -o hello.elf`, then run `load hello.elf`. Use `cc build [cc.toml]`
for projects; paths belong to the project file's directory. `cc sdk list`,
`cc sdk show HEADER`, and `cc sdk export NEW_DIRECTORY` browse or export headers.
Sources are limited to eight files totalling 64 KiB. Published native APIs and
library leases provide services; native code runs in the firmware address space.

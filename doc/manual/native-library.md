+++
id = "native-library"
title = "Native libraries"
summary = "Install reusable native libraries and manage their lifetime"
section = "api"
keywords = "native library resident module provider lease unload ABI"
packages_any = ["service_native_modules"]
agent_reference_sections = true
+++
# Native libraries

A native library supplies a reusable, versioned C API to native apps, jobs, or
firmware services. Libraries require ESP32-S3 with executable PSRAM and the
native-module service. Each loaded library has one shared instance.

## Quick reference

```text
pkg available
pkg installed
pkg unload <library-id>
pkg remove <library-id>
```

Use `pkg install <library-id>` for a library present in the signed catalog.
Installation checks the catalog signature, target, host and lifecycle ABIs,
capabilities, artifact size and hash, and the library registration. The ELF is
stored under `modules/libraries/<library-id>.elf` in the preferred volume.
`pkg installed` reports whether a library is loaded or only on storage.

Libraries are loaded when a consumer acquires their API. Acquisition reads the
installed file; it does not download code. Loaded libraries use PSRAM and remain
resident until explicitly unloaded, removed, or reclaimed while unused under
memory pressure. Installed libraries are not
loaded by the boot scan. After restarting, consumers load them again as needed.

`pkg unload` releases the resident image but keeps its installed file.
`pkg remove` unloads the library and removes the file. Both refuse while a
consumer holds a lease or the library cannot finish shutdown. Unload an unused
library before reinstalling it.

`mem reclaim` unloads unused libraries together and reports free PSRAM and the
largest free block before and after cleanup. Active leases prevent reclamation.
Application sessions, including suspended foreground apps, keep their state.

## Developer reference

Include `solar_os_native_library_abi.h`. A provider calls
`solar_os_native_library_host_v1()` from its entry point and registers one
`solar_os_native_library_descriptor_v1_t`. Its ID must match the installed
module ID. Registration declares a summary, API table, and shutdown callback;
it must not start workers or acquire resources. Library lifecycle ABI 1 is
independent of the provider's own API version.

Every provider API table starts with `uint32_t abi_version, struct_size`.
Append new fields to compatible tables. Consumers request the exact provider
ABI and enough table bytes for the methods they use. Missing or incompatible
providers return an error, a zero lease, and a null API pointer.

Native consumers request `get_service("library", 1, sizeof(*libraries))` from
their host table. The returned `solar_os_native_library_service_api_v1_t`
provides `acquire`, `retain`, `release`, and `unload`. Results use
`solar_os_native_service_result_v1_t`.

```c
uint32_t lease = 0;
const void *table = NULL;
int result = libraries->acquire("my-library", MY_LIBRARY_ABI,
    sizeof(my_library_api_v1_t), &lease, &table);
if (result == SOLAR_OS_NATIVE_OK) {
    const my_library_api_v1_t *api = table;
    /* Use api; destroy its objects and stop callbacks before releasing. */
    libraries->release(lease);
}
```

Each acquire returns an independent lease. `retain` creates another lease for
a model, client, or worker that must outlive the original consumer. Keep that
lease until its calls, objects, workers, and callbacks have finished. Release
all owned leases before the consuming module exits. Lease operations are
synchronized; the provider is responsible for synchronizing its own mutable
state. The runtime supports at most 128 outstanding library leases.
Loading and unloading share native-module admission. A request blocked by
another module operation returns `SOLAR_OS_NATIVE_ERROR_INVALID_STATE` and can
be retried when that operation finishes.

Releasing a lease invalidates its token. Releasing or retaining a stale token
fails. The API pointer is valid while at least one owned lease protects it;
never call through it after releasing all such leases.

With no leases, unloading calls `shutdown`. The provider joins its workers,
releases dependencies and allocations, then returns `SOLAR_OS_NATIVE_OK`.
A failed shutdown leaves the image loaded and allows a later retry. It must
leave the API usable when refusing shutdown. Shutdown cannot load or unload
another module while the resident operation is in progress, but can release
leases on dependencies.

Firmware consumers use `solar_os_native_library_acquire`, `retain`, `release`,
and `unload` through `solar_os_native.h`; they use the same stable result codes.
These APIs do not expose arbitrary firmware symbols to external libraries.

## C headers in library packages

Native C projects can declare exact SDK dependencies in `cc.toml`:

```toml
sources = ["main.c", "helpers.c"]
include_dirs = ["include"]
output = "build/demo.elf"
dependencies = [{ id = "jet", version = "1.12.0" }]
```

Run `cc build` beside the project file, or `cc build /dev/demo/cc.toml` from
another directory. Relative paths belong to the project file's directory.
`sources` is required; include directories and dependencies default to empty,
and the default output is `a.elf`. Projects support at most eight sources,
eight include directories and eight dependencies in an 8 KiB file. Use explicit
source paths. The format accepts single or double quoted strings, comments,
multiline arrays and inline dependency tables. Unknown or duplicate keys,
other TOML tables, multiline strings and escapes other than `\\` and `\"`
are rejected with a line number.

Builds check installed library SDK versions without loading providers or
downloading packages. Missing dependencies report `pkg install ID`; the
repository must provide the exact declared version. After compilation and ELF
validation succeed, missing output directories are created and the executable
is replaced. Build errors and cooperative cancellation preserve the existing
output. Applications still acquire and release their runtime library leases.

Libraries can embed their public C headers in the same ELF as their executable
code. Declare the exported paths in `module.toml`:

```toml
[sdk.headers]
"hello_library_api.h" = "main/hello_library_api.h"
```

Use `cmake/solar_os_native_sdk.cmake` to produce the typed artifact from the
original build ELF. The `hello-library` example shows the complete setup.
`scripts/pack_native_sdk.py` validates the declared files and adds a
non-allocated `.solaros.sdk` section. The package catalog's artifact hash covers
both code and headers. Installing, updating or removing the ELF does the same
for its headers; there are no separate header files to keep in sync.

The `cc` compiler discovers these declarations by module ID:

```c
#include <hello-library/hello_library_api.h>
```

Jet includes its public headers in the library package. Install `cc` and `jet`,
then use `#include <jet/solar_os_jet_abi.h>`. The
[native C graphics example](https://github.com/nilseuropa/solar_os/tree/main/modules/experimental/tinycc/examples/jet-cube)
shows a resumable foreground app with input and explicit library cleanup.

No manual header copy or `-I` option is needed for installed library headers.
Quoted relative includes within a packaged header stay in that library's
namespace. Headers can include published SolarOS ABI headers from the compiler
SDK or namespaced headers from other installed libraries. They must remain
compatible with the provider's versioned API table.

Browse the compiler's built-in SDK and installed library SDKs with:

```text
cc sdk list
cc sdk show solar_os_native_app_abi.h
cc sdk show jet/solar_os_jet_abi.h
cc sdk export /dev/sdk
edit /dev/sdk/jet/solar_os_jet_abi.h
```

`list` includes provider package versions and header sizes. Browsing reads ELF
metadata without activating the library providers. The built-in SDK version is
the TinyCC package version. `export` requires a new directory and preserves
the header namespaces and relative includes; an existing directory is refused.
Exported files can be browsed with `edit` or supplied through `-I /dev/sdk`.

The SDK supports up to 32 UTF-8 headers per library, 32 KiB per header and
256 KiB of text in total. Exported names include the module ID and fit in
95 bytes. Names use letters, digits, underscores, hyphens, dots and directory
separators; absolute paths and parent traversal in declarations are rejected.
Builds that share public headers can pass an explicit `--source-root` to the
packer. Declared files, including symlinks, must stay within that root.

## Runtime helpers

Providers can request these optional version-one tables through `get_service`.
Pass the size of the public table and check for NULL before use:

| Service | Header | Operations |
| --- | --- | --- |
| `time` | `solar_os_native_time_abi.h` | Monotonic microseconds and cooperative sleep on the calling task |
| `thread` | `solar_os_native_thread_abi.h` | Normal/recursive mutexes, conditions, task-local keys and joinable workers |
| `controllers` | `solar_os_native_controllers_abi.h` | Owned PID instances in the OS registry; caller supplies timestamped samples and handles its actuator |
| `libc` | `solar_os_native_libc_abi.h` | Bounded formatting, float/double parsing and math helpers |

The `libc` table appends `parse_double` (`strtod`) and `scale_double` (`ldexp`).
Consumers that need these methods must check `struct_size` through the relevant
field and check its pointer before calling it. Consumers that only need older
methods can request the table prefix ending before `parse_double` to retain
compatibility with earlier firmware. The service ABI remains version one.

Thread operations return POSIX error numbers. Destroy synchronization objects
and clear task-local values before deleting their keys. Key deletion does not
invoke destructors. Any task-exit destructor must finish while a lease still
protects its code. `worker_start` creates a worker pinned to core 0 or 1 with
an internal stack. The entry function must clear private task-local values and
return. `worker_join` waits until its code has stopped executing and releases
the stack and task storage. Keep the library lease through every worker join.

The optional `memory` table extensions report free/largest internal or external
heap blocks and identify external allocations. Check `struct_size` before using
appended members. Heap snapshots include other OS users and do not reserve RAM.

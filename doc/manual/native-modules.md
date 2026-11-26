+++
id = "native-modules"
title = "Native ELF modules"
section = "shell"
summary = "Load and manage native apps, jobs, drivers, and libraries"
aliases = ["modules"]
keywords = "native modules ELF load pkg packages install update remove catalog apps jobs drivers libraries"
packages_any = ["service_native_modules"]
+++
# Native ELF modules

Native modules use versioned public services through `get_service`. Include
`solar_os_native_client.h` for typed lookup and optional method checks. The
`system` service reports which tables the current execution can access. See
[Use the C SDK](#use-the-c-sdk).

Signed catalogs support up to 256 modules. A module can declare up to eight
exact library dependencies and 64 required system service tables.

`load` validates and runs trusted Xtensa ELF32 dynamic modules on ESP32-S3
boards with PSRAM. Paths use the calling shell's directory. It reads at most
2 MiB into executable PSRAM, checks headers and table boundaries, relocates
the image, and calls its entry with the supplied arguments. Native code shares
the firmware address space.

Modules import only `solar_os_native_host_v1`. Its table supplies ABI, target
and firmware versions, UTF-8 shell output, and optional versioned services;
see [Native media and inference](native-media.md). The original output-only
prefix remains compatible; arbitrary firmware internals are not exported.

ELFs declaring lifecycle-2 app metadata launch through the
[foreground app runtime](native-app.md), retaining their image while suspended.
Installed apps launch by module name.
Ctrl+Alt+Del, Ctrl+] or closing the session requests cooperative shutdown.
The host restarts the device if shutdown does not finish within five seconds.

Synchronous commands must stop workers and release callbacks and resources
before returning, when their code and data are unloaded. Installed jobs,
drivers, and libraries use resident lifecycles instead.

The standalone `modules/examples/hello` ESP-IDF project builds a small acceptance
module.

`pkg` verifies the catalog's ECDSA signature, SolarOS version, ESP target and
host ABI. Each ELF must match its signed size and SHA-256 and pass `load`'s
validation before staged activation. Entries declare lifecycle ABI and host
API size. Signed types select `/modules/apps/<module>.elf`, `/modules/jobs`,
`/modules/drivers`, or `/modules/libraries`.

Application lifecycle ABI 1 runs synchronous `main(argc, argv)` and unloads on
return. Job ABI 1 registers start/stop/tick callbacks; its ELF remains until
the stopped job is removed. Driver ABI 1 supports utility drivers and bound
hardware drivers. Drivers remain loaded until their devices are detached and
their providers and resource handles are closed.
Library ABI 1 supplies a versioned API protected by consumer leases. Libraries
load on demand; `pkg unload ID` releases an unused image while keeping its file.
See [Native libraries](native-library.md).
Application ABI 2 runs `main(argc, argv)` on a declared foreground worker with
versioned display, input, storage, timing and optional audio services. Resumable
apps retain code and state across session switches; see [Native apps](native-app.md).

Run `pkg` without arguments to open the package manager. It loads the saved
signed catalog without a network request and checks its signature and host
compatibility again. Apps, Jobs, Libraries and Drivers appear as collapsible
folders, including when empty. Arrows navigate the
tree, Enter opens a folder or module details, and Left returns to the parent.
Installed entries show `[I]` and incompatible entries show `[!]`.

Press `R` to refresh and save the signed catalog, `I` to install or replace the
selected module, or `U` to remove it. Install and removal require confirmation.
Operations show progress popups and a result popup; Esc cancels a download or
closes a popup. Confirmation defaults to No. The catalog and signature are
stored under `/modules/.catalog/` on the preferred storage volume. A failed
refresh leaves the previous catalog available. A catalog for a different host
firmware version is not used; press `R` after a firmware update.
After a successful catalog check, `pkg install` completion uses that verified
catalog. `pkg remove` and `pkg upgrade` completion read the installed module
directories. `pkg installed` shows versions and checks compatibility using the
saved signed catalog, without a network request.

Installed application modules are shell commands: the module ID resolves under
`/modules/apps` after built-in commands, compiled applications, and aliases.
Command completion lists installed applications. `load` remains available for
explicit paths and diagnostics. Reinstalling an application from the schema-v1
layout moves it into `/modules/apps`; `pkg remove` also recognizes the legacy
flat application path during this transition.

The job and driver acceptance modules exercise their resident paths. Use `pkg install hello-job`,
then `job start hello-job`, `job status hello-job`, and `job stop hello-job`.
Use `pkg install hello-driver`, then `expansion attach hello-driver hello0` and
`expansion detach hello0`. The hello driver claims no GPIO or bus. `pkg remove`
rejects a running job or a driver with an attached device.

For example:

```text
pkg available
pkg installed
pkg install hello
hello SolarOS
pkg remove hello
```

Compiled native components use the same package IDs and versions as their ELF
forms. `pkg installed` marks them as compiled. A matching compiled library
satisfies an exact dependency without downloading another copy, even when that
version is absent from the current catalog. `pkg install` also reuses a compiled
app and installs any library dependencies it still needs. A self-contained
compiled component needs no catalog refresh.

Compiled components cannot be overwritten, upgraded or removed with `pkg`.
Change the firmware flavor to omit one before installing its external form.
Version conflicts are reported before any files are downloaded. Upgrading an
external library also checks the pins of compiled apps that use it.

## Use the C SDK

The public headers declare service names, ABI versions, table members, error
values and resource ownership. At lookup, request the table prefix through
the last required member. Check optional methods on the returned table before
calling them.

```c
#include "solar_os_native_client.h"

int wait_briefly(void)
{
    const solar_os_native_host_api_v1_t *host = solar_os_native_host_get(
        SOLAR_OS_NATIVE_THROUGH(solar_os_native_host_api_v1_t, get_service));
    if (!host || !host->get_service) return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;

    const solar_os_native_time_api_v1_t *time = solar_os_native_time_get(
        host->get_service,
        SOLAR_OS_NATIVE_THROUGH(solar_os_native_time_api_v1_t, sleep_ms));
    if (!solar_os_native_time_has_sleep_ms(time)) return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;

    time->sleep_ms(10);
    return SOLAR_OS_NATIVE_OK;
}
```

Use `system.service_count` and `system.service_info` to inspect the available
tables. A table with size zero is unavailable to the current caller. Check
hardware capabilities and resource claims before using a device.

Keep scoped tables within the execution context that acquired them. Retain
dependent library leases while workers or callbacks can still use them. Stop
and join that work before releasing handles, leases or the module itself.
Keep resources alive if shutdown must be retried.

Use `cc sdk list`, `cc sdk show HEADER`, and `cc sdk export NEW_DIRECTORY` to
browse or export the headers installed on the device. See [C compiler](c.md).

## Update installed modules

```text
pkg update
pkg upgrade
```

`pkg update` refreshes and saves the signed catalog for the running SolarOS
version. It lists installed modules with replacements, for example:

```text
library jet: 1.11.0 -> 1.12.0 (available for upgrade)
```

This command downloads package information only. It also reports compatibility
with the current OS. Compatibility is verified when installed bytes match a
compatible entry in the current signed catalog. A different or missing entry
leaves installed compatibility unverified; this does not by itself mean the
module cannot run. An exact match to an incompatible entry is reported as
incompatible. Run `pkg update` again after a firmware update.

`pkg upgrade` uses the saved catalog, shows the changes and download size, and
asks `Apply these changes? [y/N]`. Enter cancels. Use `pkg upgrade ID` to select
one installed module and its required dependencies. Add `-y` to accept the shown
plan without a prompt, including in shell scripts. A usable saved catalog is
required; run `pkg update` first.

Versions and exact library dependencies are saved beside installed ELF files.
Older installations and manually changed files may show `unknown` as their
installed version. They are compared by content hash and, if different,
offered for replacement with the current catalog package. A matching hash
avoids downloading an unchanged module. A changed artifact with the same
version can be upgraded, for example after a firmware update. Known newer
versions and version labels whose order cannot be determined are held back;
use `pkg install ID` when you intend an explicit replacement. Automatic version
ordering supports numeric dotted versions such as `1.12.0`.

Close app sessions and unload affected resident modules before upgrading.
Stop jobs or detach driver devices first, then use `pkg unload ID` to keep their
installed files while releasing their registrations. In-use libraries refuse
to unload until their clients release them. The upgrade checks active modules
and exact dependency requirements before downloading. A shared library change
is refused if it would break an installed consumer that is not being upgraded,
or if that consumer's requirements cannot be verified.

All upgrade artifacts are downloaded and validated before the ELF files and
their version records are replaced together. Ctrl+C, Esc, or the app-exit key
cancels preparation. Failed downloads and cancellation preserve installed
files. Publication rolls the group back on an I/O error; this is not a
power-loss recovery journal. New jobs and drivers are registered after
publication; libraries load when needed.

These commands manage downloadable native modules. Firmware features listed
by `pkg system` are updated with the firmware.

## Quick reference

Use `load PATH [ARGS...]` to run a trusted native ELF module directly. Run `pkg`
to browse the signed module catalog, `pkg install ID` to install a module, and
`pkg remove ID` to remove it. Installed apps run by module ID. Stop jobs and
detach driver devices before removal; `pkg unload ID` releases an unused native
library image while keeping its installed file. `pkg unload ID` also releases a
stopped job or detached driver. Use `pkg update` to check for module upgrades and
`pkg upgrade [ID]` to apply them after confirmation.

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
the stopped job is removed. Driver ABI 1 registers a zero-binding utility
driver and remains until its devices are detached and the module removed.
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
catalog. `pkg remove` completion and `pkg installed` read the installed module
directories directly.

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

## Quick reference

Use `load PATH [ARGS...]` to run a trusted native ELF module directly. Run `pkg`
to browse the signed module catalog, `pkg install ID` to install a module, and
`pkg remove ID` to remove it. Installed apps run by module ID. Stop jobs and
detach driver devices before removal; `pkg unload ID` releases an unused native
library image while keeping its installed file.

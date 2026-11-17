# TinyCC compiler library

TinyCC supplies the versioned compiler API used by `cc` on ESP32-S3 devices
with PSRAM. Install the `cc` package for the command and its matching library.
The package declares the firmware memory, worker, time and C helper tables it
requires, so incompatible firmware is rejected during installation.

The optional `cc` firmware group links the same compiler and command into an
ESP32-S3 image. Library acquisition selects this built-in provider without an
installed ELF. The usual leases and busy checks protect its worker; unloading
releases its runtime snapshot while the code and static state remain in the
image. Private compiler, allocator and frontend symbols are isolated from the
firmware and from each other. All standard flavors leave this group disabled.

The provider compiles bounded C translation units into loadable SolarOS ELF
modules. It uses a private heap, cooperative cancellation, a stack guard and
explicit limits on sources, headers, output and compile time. It loads when a
consumer acquires its API and can be unloaded after all leases are released.

The [compiler reference](../experimental/tinycc/README.md) describes the C
subset, limits, API, pinned upstream sources and build tools.

# TinyCC compiler library

TinyCC supplies the versioned compiler API used by `cc` on ESP32-S3 devices
with PSRAM. Install the `cc` package for the command and its matching library.
The package declares the firmware memory, worker, time and C helper tables it
requires, so incompatible firmware is rejected during installation.

The provider compiles bounded C translation units into loadable SolarOS ELF
modules. It uses a private heap, cooperative cancellation, a stack guard and
explicit limits on sources, headers, output and compile time. It loads when a
consumer acquires its API and can be unloaded after all leases are released.

The [compiler reference](../experimental/tinycc/README.md) describes the C
subset, limits, API, pinned upstream sources and build tools.

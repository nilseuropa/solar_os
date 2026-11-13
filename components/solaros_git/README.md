# SolarOS Git engine

This ESP-IDF component builds libgit2 1.9.7 at commit
`49e408b3208bc3093757a1c2db938d3590f3f412`. The top-level build downloads an
archive verified by SHA-256. Upstream sources and licenses stay in the build's
`_deps/libgit2` directory. libgit2 uses GPL v2 with its linking exception; its
bundled PCRE2, zlib, llhttp, xdiff and SHA1 collision detection sources retain
their upstream notices. See the pinned source's `COPYING` and dependency
license files when distributing the firmware or dependency sources.

The component uses libgit2's buffered mmap emulation, collision-detecting SHA1,
bundled compression/regex/diff/HTTP parser, and mbedTLS SHA256. Library calls are
serialized by the Git service; pack workers and subprocess execution are disabled.
Git allocations use SolarOS memory policy. `patch_sources.py` generates small
ABI corrections for Xtensa's `int32_t` typedef; downloaded sources remain
unchanged. The source-list excludes the PCRE2 fuzzing harness.
The SHA1 collision-detection vectors are declared read-only so their 11,352-byte
lookup table stays in flash instead of consuming internal SRAM at boot.
Mutable library globals use PSRAM. Zero-initialized state uses external BSS;
initialized globals have typed defaults in flash, copied once before the first
library initialization or options call. This preserves pointers, defaults and
function-local static state across subsequent init/shutdown cycles. A linker
fragment keeps the initialization records in flash and maps library BSS to
PSRAM. The service's serialization guard and flash-writing worker stack remain
internal; no Git state is accessed by an ISR or with caches disabled.
The regex compiler's 12 KiB scratch workspace uses the Git allocator, keeping
its stack frame within the foreground worker's budget. Cleanup covers every
return path. Allocation and progress paths yield periodically to the RTOS.
Streaming file/filter copy buffers use 2 KiB instead of upstream's 64 KiB stack
arrays. The non-threaded TLS key table reuses disposed slots so repeated
commands can initialize and shut down the library without exhausting the table.
The unused PCRE2 DFA engine is excluded. A compiler frame-size limit rejects
individual library stack frames larger than 16 KiB.

The port maps lstat to stat, treats chmod as a file-existence check, supplies
ESP hardware entropy, and resolves absolute VFS paths without a process cwd.
It disables FAT's copying emulation of hard links so Git uses rename/copy fallbacks.
FatFs metadata replacement keeps a backup and restores it after an ordinary
rename error. It deliberately fails if a previous backup still exists and does
not claim atomicity across power loss. Symlink/submodule checkout is rejected
by the service. External attribute filters fail explicitly.

SSH uses the SolarOS SSH transport with an exec channel for upload-pack or
receive-pack. HTTPS uses libgit2's HTTP protocol with a custom ESP-TLS stream
and the system certificate bundle. It does not use upstream libgit2 TLS or SSH
backends. Firmware capability gates select this service only on Wi-Fi/PSRAM
boards. The synchronous service API is available to native callers; dedicated
Python and Lua bindings are not provided.

Host interoperability tests:

```sh
SOLAR_OS_LIBGIT2_SOURCE_DIR=/path/to/pinned/libgit2 python3 tests/test_git_client.py -v
```

These tests build the production Git service against host libgit2 and use an
isolated local git-daemon for pack transfer and rejection tests. They do not
validate the ESP VFS, SSH or TLS adapters; those need device testing.

Verify a linked ESP32-S3 firmware's PSRAM state and initialization records with:

```sh
/home/nils/.platformio/penv/bin/python components/solaros_git/check_memory.py .pio/build/t_deck_plus/firmware.elf
```

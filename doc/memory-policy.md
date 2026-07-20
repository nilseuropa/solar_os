# Memory Policy

SolarOS uses ESP-IDF's capability heaps directly. The memory policy does not
replace the allocator; it gives subsystems an explicit allocation intent and
prevents an optional PSRAM allocation from consuming memory needed for tasks,
network transports, and DMA.

Allocation classes:

- `internal-critical`: internal byte-addressable RAM for control structures and
  work that must remain available during memory pressure.
- `internal-preferred`: internal RAM while preserving the configured reserve,
  then PSRAM. Use this for latency-sensitive buffers that can still operate
  from external memory under pressure.
- `dma`: internal DMA-capable RAM.
- `external-required`: PSRAM only. Failure is reported to the caller.
- `external-preferred`: PSRAM first. If the board has PSRAM, internal fallback
  is limited to 4 KiB allocations and must preserve a 32 KiB contiguous
  internal reserve. Boards without PSRAM retain an unrestricted internal
  fallback so the same core services can still run.
- `transient`: short-lived scratch storage with the same guarded placement as
  `external-preferred`. Its lifetime distinguishes it for later reclamation and
  budgeting work.

Every policy allocation records its class and requested size. Failures record a
short subsystem tag and a snapshot of free and largest blocks in the log. Use
`mem policy` to inspect heap regions, class counters, fallback counts, the
configured reserve, and the most recent tagged failure.

New code should use `solar_os_memory_alloc()`, `solar_os_memory_calloc()`, or
`solar_os_memory_realloc()` with an explicit class and a short subsystem tag.
Apps, jobs, shell commands, and services must not call capability allocators
directly. This keeps placement, reserves, fallbacks, failure reporting, and
class statistics in one place.

Direct capability-heap allocations are reserved for low-level cases whose
placement is intrinsic rather than a fallback preference. Hardware drivers may
retain them for DMA buffers, device-facing staging buffers, and display
framebuffers, with a short comment explaining the required capability. For
example, SD sector and SPI line buffers require internal DMA memory, while
optional display shadow frames can explicitly prefer PSRAM.

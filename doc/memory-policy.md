# Memory Policy

SolarOS uses ESP-IDF's capability heaps directly. The memory policy does not
replace the allocator; it gives subsystems an explicit allocation intent and
prevents an optional PSRAM allocation from consuming memory needed for tasks,
network transports, and DMA.

Allocation classes:

- `internal-critical`: internal byte-addressable RAM for control structures and
  work that must remain available during memory pressure.
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
`solar_os_memory_realloc()` with an explicit class. The older
`solar_os_psram_*()` helpers remain available for existing services and now
route through `external-preferred`; they no longer silently use a large internal
block after a PSRAM allocation failure.

Direct capability-heap allocations are reserved for low-level cases whose
placement is intrinsic rather than a fallback preference. The RAMFS arena is
PSRAM-required, and the optional ILI9341 shadow framebuffer disables its
partial-update optimization when PSRAM is unavailable instead of consuming
internal RAM.

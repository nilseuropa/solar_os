# Memory Policy

SolarOS uses ESP-IDF's capability heaps directly. The memory policy does not
replace the allocator; it gives subsystems an explicit allocation intent and
prevents an optional PSRAM allocation from consuming memory needed for tasks,
network transports, and DMA.

On a PSRAM-enabled board, ordinary `malloc()` allocations, Wi-Fi/lwIP buffers,
and Bluetooth host allocations prefer PSRAM. Internal SRAM is the exception:
it is reserved for DMA, ISR-adjacent state, cache-disabled flash operations,
and other explicitly internal work. On a board without PSRAM the same APIs use
internal SRAM and may report out of memory when the workload exceeds it.
Classic ESP32 targets cannot safely place task stacks in PSRAM. ODROID-GO also
keeps its smaller Wi-Fi/lwIP allocation profile because the PSRAM-first adapter
does not fit its IRAM segment; its general, queue, and Bluetooth allocations
still prefer PSRAM.

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

## FreeRTOS objects

ESP-IDF deliberately allocates ordinary dynamic FreeRTOS task stacks and queues
from internal SRAM. SolarOS therefore owns their placement:

- A PSRAM build reserves one 16 KiB internal transient-executor stack during
  early boot, before optional jobs and peripherals can fragment the internal
  heap. Its queue is PSRAM-backed. Python, Lua, email synchronization, and
  future transient operations submit callbacks to this executor instead of
  creating another task. Work is serialized; if the executor is busy, another
  operation waits in PSRAM rather than consuming another internal stack.
- Long-lived services reserve their indispensable internal stacks at boot.
  The chat gateway reserves one reusable 8 KiB worker. `chat-sync` itself is a
  bounded scheduler tick and does not own a task.
- `solar_os_task_create_pinned()` uses an internal stack. This is the safe
  default because filesystem, NVS, TLS, OTA, and other library paths can enter
  cache-disabled flash operations.
- `solar_os_task_create_pinned_external()` uses a PSRAM stack when supported
  and otherwise falls back to an internal stack. It is an explicit opt-in only
  for a worker audited never to initiate flash access or otherwise run while
  the external-memory cache is disabled. Pair it with
  `solar_os_task_delete_external()`.
- `solar_os_queue_create()` places queues in PSRAM whenever the board has
  PSRAM, otherwise it creates a normal internal queue. Pair it with
  `solar_os_queue_delete()`.
- `solar_os_task_create_pinned_internal()` documents a strict internal-stack
  contract for cache-disabled, DMA, ISR-adjacent, or timing-sensitive work.
  `solar_os_queue_create_internal()` is the matching explicit queue exception.
  Pair them with their `_internal` deletion functions.

Apps, jobs, services, and shell commands must not call dynamic FreeRTOS task or
queue creation directly. New workers and queues use the automatic SolarOS API
unless an internal-memory requirement is documented at the call site.

New short-lived or foreground operations should use `solar_os_work_submit()`.
They must stop cooperatively, and must never delete the shared executor task.
Use a dedicated task only for concurrent, continuously running work that
cannot be represented by a bounded scheduler tick. Reserve indispensable
dedicated stacks during service initialization; optional dedicated tasks may
still report out of memory when internal SRAM is exhausted. On boards without
PSRAM the executor is created lazily, so the existing user-managed memory
tradeoff remains.

`mem policy` reports executor initialization, stack size and high-water mark,
queued/running work, and submitted/completed/cancelled/rejected totals. These
counters make queue pressure and future executor sizing observable without
changing client code.

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

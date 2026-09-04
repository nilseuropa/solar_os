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
does not fit its IRAM segment. Its board configuration maps the ESP32 SRAM1
region as IRAM, adding 40 KiB of instruction space; the CI/deployment build
requires at least 32 KiB to remain free. This image layout requires a compatible
ESP-IDF bootloader, so devices carrying an older bootloader must receive one
factory/full flash before later application-only or OTA updates. General,
queue, and Bluetooth allocations still prefer PSRAM.

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

## Foreground application state

Foreground applications are cold until they are launched. An application must
not reserve mutable SRAM or PSRAM merely because its package is present in the
firmware.

The normal implementation puts the application state behind a file-scope
pointer and declares `state_slot`, `state_size`, and `state_storage` in its
`solar_os_app_t` descriptor. `solar_os_app_start()` allocates and zeroes that
state immediately before the app's `start()` callback. The state remains valid
while the app is suspended so resumable sessions keep their contents.
`solar_os_app_stop()` releases it after `stop()` returns, and failed starts
release it as well. Display sessions and UART, USB CDC, Telnet, and other port
shells use this same lifecycle boundary.

An app can retain custom dynamic allocations when its lifecycle requires more
than one allocation or a size that is known only at launch. Those allocations
must still be created no earlier than `start()` and released by `stop()` or the
failed-start path.

If a worker can outlive `stop()`, the descriptor supplies
`state_release_ready`. The callback returns true only when no worker can access
the state. `state_release_cleanup` supplies any idempotent queue, file, or
service cleanup that must run before the state block is freed. A timed-out stop
retains the state instead of creating a use-after-free; the next launch reclaims
it after the worker reports completion.

Concrete mutable file-scope and function-local static objects are forbidden in
foreground app translation units. `EXT_RAM_BSS_ATTR` is not an exception: it
still consumes memory before launch and falls back to internal SRAM on boards
without PSRAM. A static pointer can hold a cold state slot or dynamically owned
resource. A concrete static object is allowed only when it must remain in SRAM,
must be shared outside one app launch, or is part of the always-running boot
shell. Mark each such object immediately before its declaration with
`SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("specific reason")`.

Run `python3 scripts/check_foreground_app_state.py` to enforce this rule. The
Python test suite runs the same check for every source file that declares a
foreground app and for app-owned helper modules listed by the checker. New
foreground launch paths must call `solar_os_app_start()` and
`solar_os_app_stop()` rather than descriptor callbacks directly.

Every policy allocation records its class and requested size. Failures record a
short subsystem tag and a snapshot of free and largest blocks in the log. Use
`mem policy` to inspect heap regions, class counters, fallback counts, the
configured reserve, task-admission counters, pending launches, and the most
recent tagged failure.

## Task admission

PSRAM does not make an arbitrary FreeRTOS task stack safe in external memory.
Filesystem, NVS, TLS, OTA, and other library paths can run while the external
memory cache is unavailable. SolarOS therefore keeps general task stacks in
internal SRAM and uses PSRAM for application heaps, queues, buffers, and
pending-launch records.

Every SolarOS-owned task launch goes through one serialized admission boundary
and declares a role:

- `system` is boot and indispensable runtime infrastructure.
- `foreground` is interactive or explicitly requested work. It is admitted
  against its actual declared stack requirement and may consume the shared
  reserve.
- `background` is optional work. It must leave the shared 32 KiB internal
  reserve available for foreground launches.

Apps and jobs that own workers declare their stack requirement in their
descriptor. Built-in internal foreground stacks are compile-time checked
against the 28 KiB supported maximum. ESP-IDF-managed HTTP and MQTT workers use
the same serialized admission boundary.

If a background job cannot preserve the reserve, a PSRAM-enabled board records
the complete start request in PSRAM, reports the job as `waiting`, and retries
it from the scheduler when internal stack memory becomes available. Stopping or
restarting the job cancels or replaces that request. If PSRAM is absent, the
pending record cannot be created and the start returns `ESP_ERR_NO_MEM`, leaving
memory balancing to the user.

This policy is dynamic: it does not reserve a large permanent executor stack
and does not serialize unrelated Python, Lua, email, or chat work through one
worker. `mem policy` reports requests, admissions, denials, launch failures,
the current number of waiting jobs, successful delayed launches, and cancelled
waits.

Headless configurations preallocate one 10 KiB port-shell worker because it
immediately becomes their primary interactive console. Display-capable systems
create UART and USB CDC port-shell workers on demand, leaving that internal
stack unallocated until a port shell is requested.

## FreeRTOS objects

ESP-IDF deliberately allocates ordinary dynamic FreeRTOS task stacks and queues
from internal SRAM. SolarOS therefore owns their placement:

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

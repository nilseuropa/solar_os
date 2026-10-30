# Memory Policy

SolarOS uses ESP-IDF capability heaps for internal RAM and constrained driver
allocations. On boards with sufficient PSRAM, it reserves one contiguous region
at boot for bulk buffers and resident objects. Bulk scratch and large unconstrained SDK buffers grow from one end;
executable images and persistent terminal metadata grow from the other.
Ordinary pointers keep their addresses. Adjacent freed blocks merge automatically.
Allocation failure triggers shared pressure handling: compact eligible buffers,
ask registered owners to release rebuildable data, compact released holes, then
retry once. This applies to existing apps, jobs, services, native host allocations
and eligible SDK PSRAM allocations. Internal and DMA policy allocations also ask
reclaimers before final failure. Task admission reclaims before its launch lock.

`SOLAR_OS_PSRAM_GENERAL_RESERVE_BYTES` leaves 512 KiB outside the region by default
for capability-constrained SDK and driver allocations; zero disables the region.
Ordinary SDK PSRAM allocations can overflow into the managed region. DMA,
ISR, and stronger alignment requirements retain their capability-heap path.
`SOLAR_OS_PSRAM_BULK_MIN_BYTES` defaults to 64 KiB. Smaller policy allocations
prefer the capability heap and overflow at the resident end when needed.
`mem` reports aggregate free PSRAM, its largest block, and managed-region status.
Allocation can still fail if no available block satisfies a request.
Large requests prefer the managed region, then use the SDK PSRAM heap if they
cannot fit there. `mem policy` also reports free and largest blocks in the SDK
heap separately. Allocation searches skip live managed objects; freeing an
object locates its header directly and merges adjacent holes.

Managed reallocations shrink in place and return their unused tail to the
region, or grow into an adjacent free block. They allocate and copy only when
the existing block cannot grow. A failed resize preserves the original buffer.
Raw pointers remain stable unless their owner explicitly reallocates them.
Compaction moves only explicitly movable handle payloads. Pinned objects remain
barriers: free bytes on opposite sides cannot necessarily satisfy a contiguous
request. Failure still returns NULL or `ESP_ERR_NO_MEM`.

Both small and bulk PSRAM allocations try the SDK heap and managed region
before shared pressure handling and retrying once. Native foreground
admission also reclaims and rechecks before reporting insufficient memory.
The task holding the native loader operation can reclaim idle libraries during
ELF allocation; other tasks remain excluded from loader operations. Active
library leases and shutdown refusals prevent reclamation. ELF code/data
allocation failures report `ESP_ERR_NO_MEM`.

## Ownership and fragmentation

Every feature has a pinned default contract, including existing native modules,
interpreter heaps, providers and SDK objects. Code, stacks, DMA, ISR state and
objects with borrowed raw pointers keep fixed addresses. Arbitrary C pointer
graphs cannot safely participate in transparent moving compaction.

Use `solar_os_memory_buffer_create()` for byte buffers that can lend temporary
pointers. Match every `buffer_pin()` with `buffer_unpin()` on all return paths.
Pointers stay stable across nested pins and expire after the final unpin.
Compaction updates handles under the arena lock. Destruction refuses outstanding
pins; owners serialize handle lifetime against other callers. Payloads outside
the managed region remain fixed. Displayd pins its HTTP response copy through
the complete send, then permits movement while idle.

Use `solar_os_paged_array` for indexed metadata without a contiguous-storage
requirement. Records occupy pages of at most 8 KiB, indexed by a separate pointer
directory. Growth preserves existing record addresses; failed growth frees only
new pages and preserves the old array. Reader and Writer share this container
for document lines and runs, including rotation and zoom reflow. Source text
and other pointer graphs retain their own pinned contracts and may still report
insufficient contiguous memory.

`solar_os_memory_register_reclaimer()` accepts a named, caller-owned descriptor.
Lower priorities run first. Callbacks run outside allocator locks, use
nonblocking ownership checks, and release only rebuildable idle data. They must
not wait for another allocating worker, launch tasks or retain the request
pointer. Descriptors are immutable while registered. Unregister returns false
while a pressure pass references the descriptor: retain its storage and callback
code, then retry outside allocator locks. Recursive/concurrent passes are
skipped. Allocation pressure skips remaining owners once the request fits;
manual `mem reclaim` asks all owners. Active library leases prevent unloading.

Reader and Writer register their derived document layouts only while suspended.
Any pressure request, including another app's launch admission, can release
those pages. Parsed documents, unsaved editor text, cursors and source-position
anchors stay resident. Resume or close unregisters and joins broker references
before touching the layout or freeing the app's state. Resume rebuilds the
layout and restores its source anchor. If registration is unavailable, suspend
drops the derived layout immediately. Active document caches are never reclaimed.

Native foreground apps can register an optional `set_memory_reclaimer` hook.
The host enables it after suspend joins audio/presentation workers. Resume
closes this gate and waits for in-flight reclamation; cleanup also unregisters
before module unload. This hook runs on the pressure caller and must only free
owner-safe cache entries. DOOM releases unlocked WAD mappings through it; locked
engine pointers stay valid. Old modules retain their pinned contract; using the
hook requires a rebuilt module.

The native memory service appends opaque buffer operations to ABI 1. Check
`struct_size` before using optional fields. Release pins and destroy handles
before module unload. Lua continues to use tagged allocation policy;
MicroPython collects within its fixed VM heap. VM heaps stay pinned to the host.

## Diagnostics and coverage

`mem compact` moves eligible payloads without eviction. `mem reclaim` reports
recovered bytes and free/largest PSRAM before and after. `mem owners` reports
managed-region bytes, counts, movable bytes and largest object by copied owner
tag. Untagged SDK objects appear as `sdk.pinned`; SDK-region totals remain in
`mem policy`. Owner diagnostics are best effort, with 512 names truncated to
23 characters. `mem policy` also reports pressure passes, callbacks, busy passes
and released/moved bytes.
Pressure recovery checks at least 4 KiB of current task-stack headroom before
entering the broker and owner callbacks. Shallow workers skip reclamation and
report allocation failure; `low-stack` counts these skips. This guard protects
the shared recovery path. It does not make an arbitrary callback or unrelated
recursive code safe. Reclaim callbacks must keep their own stack use bounded.

`scripts/check_memory_contracts.py` inventories every package and owned source
file, including native ports and files without allocation calls. PlatformIO runs
it and writes `memory-contracts.json` into each environment's build directory.
Existing raw capability/SDK/RTOS and host-test sites have exact API counts and
pinned reasons in `packages/memory_contracts.json`. Adding a raw site, including
one inside an existing feature, fails the check until it uses shared APIs or
has a reviewed pinned exception. Vendor/interpreter boundaries are explicit.
This lexical check shows coverage; it does not prove every runtime failure path
or every combination of features safe.

The ESP-DL backend keeps weights and tensor metadata resident, but leases its
activation workspace only for execution. It copies outputs before releasing
the lease. Preprocessing can use that space while the model is idle. This
requires an updated external ESP-DL library on firmware using that provider.

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
- `external-resident`: PSRAM only, placed at the resident end of the managed
  region regardless of size.
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
recent tagged failure. A task failure retains its allocation-time free and
largest-block snapshot, so later heap recovery does not hide fragmentation at
the point of failure.

## Classic ESP32 PSRAM access

Classic ESP32 builds supporting silicon before revision 3 select the SDK's
duplicate load/store PSRAM workaround for code compiled across the OS and SDK.
Existing generated configurations migrate to this strategy at configure time;
a build guard also checks the SDK's derived volatile-spinlock setting. ESP32-S3,
other chip families and configurations requiring revision 3 or newer retain
their own settings.
Precompiled vendor libraries retain the SDK's supplied workarounds.
The SDK disables incompatible hardware crypto acceleration for this strategy;
the corresponding cryptographic operations use their software implementations.

This protects PSRAM state against lost updates from nearby stores and reloads
on affected silicon, including filesystem control objects. Keep the compiler
workaround enabled wherever external memory is used. See Espressif's
[CPU-3.9 erratum](https://docs.espressif.com/projects/esp-chip-errata/en/latest/esp32/03-errata-description/esp32/cpu-read-and-write-errors-related-to-access-sequence.html).

## CPU work budgets

Long synchronous work uses the shared `solar_os_work_budget_t` checkpoint API.
Document parsing, layout and teardown, static HTTP transfers, and MP3 playback
check their budgets between bounded work units. After an 8 ms slice, a
checkpoint blocks for at least one full scheduler tick before starting the next
slice. Short operations continue without sleeping. This allows lower-priority
and idle tasks to run under concurrent document, storage, network and audio load.

Each operation owns its budget. Checkpoints run in task context, outside
critical sections and locks needed by other workers. They do not dispatch app
events, change task affinity or priority, or feed or relax the watchdog. A
checkpoint cannot bound a single blocking driver call or work unit; new long
loops still need checkpoints at suitable boundaries.

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

Headless configurations preallocate one 16 KiB port-shell worker because it
immediately becomes their primary interactive console. Display-capable systems
create UART and USB CDC port-shell workers on demand, leaving that internal
stack unallocated until a port shell is requested.

## FreeRTOS objects

Frame presentation workers require at least 8 KiB of internal stack for display
conversion, allocation/reclamation, and error logging. A display target can
request a larger stack. If the full stack cannot be admitted, presentation
startup returns `ESP_ERR_NO_MEM`.

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

# Service concurrency contract

SolarOS shell sessions, background workers, and the display shell run in
different FreeRTOS tasks. A service API must therefore assume that any other
service API can be called concurrently unless its header explicitly says
otherwise.

## Registry rules

- Protect slot allocation, lookup, publication, ownership, reference counts,
  and state transitions with the registry's metadata lock.
- Keep metadata lock sections bounded to validation and copying. Never invoke a
  driver, app, job, shell, or user callback while holding a global registry
  lock. Do not perform port, bus, display, filesystem, network, NVS, or other
  potentially blocking I/O while holding it.
- Reserve a slot before starting external lifecycle work. Publish it only after
  attach/start succeeds. A reserved or retiring name remains unavailable until
  that transition completes, so concurrent callers cannot create duplicates.
- Copy query results into caller-owned snapshots. A pointer stored in a
  snapshot is informational unless a separate claim API keeps its owner alive.
- Give each reusable slot or lifecycle incarnation a nonzero generation.
  Completion from an old generation must not update a reused slot. Keep a
  reference count while callbacks or borrowed objects are in flight, and reject
  removal while references remain.
- Establish one lock order when an operation spans services. Acquire only the
  metadata needed to obtain a generation-checked snapshot or claim, release the
  registry lock, then call the next service. This prevents registry-to-driver
  and cross-service lock inversion.

## Lifecycle ownership

Lifecycle entry points should run on the main task when practical. APIs that
must accept calls from shell or worker tasks serialize the transition in the
owning service and use a reserved state while callbacks run.

Job start, stop, and tick callbacks run without the jobs registry lock. The
registry waits for in-flight tick references before start or stop invokes the
job lifecycle callback. A worker that completes asynchronously records the job
generation it was started under and passes that generation to
`solar_os_jobs_mark_stopped()`; stale completion is rejected.

Expansion attach and detach reserve the device name while resource, bus, and
driver operations run. Device enumeration returns copies and does not expose
registry storage.

Display target claims are references. Repeated claims by the same owner require
matching releases, and a target cannot be unregistered while a claim or an
internal driver snapshot is active. `solar_os_display_open_gfx()` keeps the
returned graphics pointer valid through that claim.

Port-shell slots carry an internal generation across task creation and cleanup.
The shell task owns its session and I/O objects; the registry lock protects only
slot identity, task/lifecycle flags, and list snapshots. Cleanup performs app
callbacks and terminal/port I/O first, then generation-checks the final slot
retirement.

## Callback requirements

Callbacks may call other SolarOS services because no global registry lock is
held around them. A callback must not synchronously start or stop its own job;
asynchronous completion uses `solar_os_jobs_mark_stopped()` instead. Callbacks
must not retain pointers to registry slots or caller-owned snapshot buffers.

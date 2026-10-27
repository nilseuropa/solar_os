+++
id = "native-app"
title = "Native foreground apps"
summary = "Install native apps, switch sessions, and use the foreground module API"
section = "api"
keywords = "native app foreground emulator apple2 gameboy suspend resume resources"
packages_any = ["service_native_modules"]
agent_reference_sections = true
+++
# Native foreground apps

Native foreground apps are trusted ESP32-S3 modules. They use the same sessions,
display ownership, keyboard shortcuts and cold-state lifecycle as built-in apps.
Emulators such as [Apple II Plus](app.apple2.md), [Game Boy](app.gameboy.md)
and [amiga](app.amiga.md)
are foreground apps. Their CPU cores and guest state belong to their modules.

## Quick reference

```text
pkg installed
pkg install apple2
pkg install gameboy
apple2 /sdcard/apple2/dos33.nib
gameboy /sdcard/roms/game.gb
session create apple2 display0 /sdcard/apple2/dos33.nib
load /sdcard/apple2.app.elf /sdcard/apple2/dos33.nib
```

## Using installed apps

Catalog entries use `type = "app"` and `lifecycle_abi = 2`. `pkg install ID`
installs an ELF under `modules/apps/ID.elf` in the preferred storage volume.
Run `ID [arguments]` from a display shell, or use
`session create ID display0 [arguments]`. `load /path/app.app.elf [arguments]`
also recognizes foreground metadata. The filename does not determine the
lifecycle of an app that declares metadata. Lifecycle-1 apps remain synchronous
commands; their code is unloaded when `main` returns.

Relative media paths use the launching shell's current directory, including
when `session create` launches an app on a display from a port shell. Apps
that declare file extensions also receive the shell's normal file and directory
Tab completion for their arguments.

Discovery reads ELF headers, dynamic symbols and the foreground manifest without
loading the whole app into memory. Launch validates the complete ELF and checks
that its manifest matches the session's admitted resource declaration.

Installed foreground apps appear in app discovery. An installed foreground app
with a built-in app's ID takes precedence while installed. Each app can own one
session. Different external apps can remain loaded in suspended sessions.
Installing or removing a running or suspended foreground app is refused.
Resource declaration changes to an app already discovered require a restart.

App lists and command completion share cached discovery. Package installs and
removals, FTP changes, and storage commands refresh it on the next lookup.
Changes made through direct file APIs may take up to 30 seconds to appear.

The package catalog has application, job, driver and
[library](native-library.md) categories.

## Developer reference: resource declaration

`solar_os_native_app_abi.h` defines the exported, pointer-free
`solar_os_native_app_manifest` record. It declares the app ID, summary, GUI or
TUI class, resumable flag, worker stack and priority, internal-memory and PSRAM
minima, event tick interval, and optional file extensions. File extensions are
space-separated lowercase suffixes such as `.gb`; opening a matching file passes
its path as the first argument. The host validates this record before executing
module code and checks it again when loading the session worker. Catalog
lifecycle and ID must match the ELF metadata.

The declared memory minima are admission checks, not reserved heaps or quotas.
Bulk guest state and executable images use PSRAM. An app owns its allocations
and frees them before returning. Worker resources are allocated on launch;
no app-specific startup hook runs at boot.

Large OS buffers use a managed PSRAM region that groups bulk buffers separately
from resident executable images. `mem` shows its free space and largest block.
Reclamation releases unused libraries; it does not discard running or suspended
app state. A workload can still be refused when its allocations do not fit.

## Suspend, resume and close

`Alt+Tab` and `Alt+Right` select the next session; `Alt+Left` selects the previous
session. Display shells also provide `session fg ID`. A resumable module polls
`suspend_requested` at safe points, including while paused, and calls `suspend`.
The host joins presentation and closes foreground audio before acknowledging the
switch. Guest state, the ELF image and its worker remain allocated. Resume
restores display ownership, clears held input and allows the worker to continue.
The module rebases its timing, restores optional audio and submits a full frame.

Ctrl+Alt+Del, Ctrl+] or `session close ID` requests cooperative shutdown.
The module polls `stop_requested`, saves any state required by its app, stops its
callbacks, frees allocations and returns. The host joins presentation and audio
before unloading the image. A worker that fails to suspend or close within five
seconds causes a device restart; it is never forcibly deleted while executing
module code. Native modules run in the firmware address space.

## Foreground service

`main(argc, argv)` obtains `solar_os_native_host_v1()` and requests
`get_service("foreground", 1, required_size)`. Check the returned table and its
size before using appended methods. Calls belong to that app's worker; tables
cannot be used by another app or a shell task.

The service provides time and bounded sleep, display palette and asynchronous
frame submission, input events including release and reset, path resolution,
bounded file reads and atomic replacement, optional PCM audio, exit status,
and cooperative suspend and stop. Input events carry logical key bytes defined
by `SOLAR_OS_KEY_*` in `solar_os_key_codes.h`, alongside physical key identity,
HID usage, modifiers, and press/release state. Each app has its own input queue,
presenter, audio ownership and retained state. OS session shortcuts pass through
captured keyboards. Modules must not leave callbacks or tasks running after
`main` returns.

Physical keyboards and buttons retain their actual press/release timing.
The `pointer-arrows` job supplies held arrow keys while its pointer moves and
releases them when movement stops. Logical taps from character-only input have zero
physical identity and HID usage. `read_key` delivers their press immediately
and a synthetic release after 140 ms, measured from consumption by the worker.
This keeps taps visible to frame-based guests even when the producer queues
both halves together. Repeated taps of the same key extend its pulse. Input
reset clears pending pulses; modules must clear held controls on reset.

The appended `video_dimensions(&width, &height)` method reports the calling
app's rotated foreground graphics dimensions before video is opened. Modules
that render text or other display-sized content can allocate a matching frame
instead of assuming a board's resolution. Request a service table large enough
to include this method. Fixed-resolution emulators can still submit their own
frame size and use the presenter's aspect-preserving fit.

`present` copies a submitted frame. For rendering into host-owned buffers, check
for `video_acquire` and `video_exchange`. Acquire once after opening video, fill
the returned buffer, then exchange it to queue the frame without a pixel copy.
Exchange returns the next writable buffer, whose previous contents are
unspecified. Each submitted buffer remains readable until the next exchange;
do not write to it. Never free these buffers or mix this path with `present`.
The writing buffer survives suspension, and the host restores the latest frame
on resume. All host-owned buffers are released when the worker exits.

Use `cooperate` at regular CPU-work checkpoints. The host coalesces these
requests while allowing periodic idle-task work. Use `sleep_ms` for timed waits,
and continue polling stop and suspend requests. The
`solar_os_native_foreground_cooperate` helper falls back to a short sleep on
hosts without this method.

The appended `disk_open`, `disk_read`, `disk_write`, `disk_sync`, and
`disk_close` methods provide exclusive access to fixed-size raw disk images.
Request a table through `disk_close` before using them. `disk_open` returns an
opaque handle and byte size; reads and writes take byte offsets and at most
64 KiB per call. Writes sync before returning. Claims survive suspension, and
the host closes remaining handles before unloading the worker. Unmount a host
FAT image before opening it as a guest disk.

Apps that set `SOLAR_OS_NATIVE_APP_POINTER_EVENTS` receive display-routed
pointer events through the appended `read_pointer` method. Absolute positions
use the rotated host surface; relative deltas are unscaled. Buttons are a
held-state mask (bit 0 left, bit 1 right). Each opting-in app has a bounded
pointer queue. Queue overflow and session switches produce a reset event;
the module must release buttons and clear position anchors on reset.

The appended `calendar_read` method returns the host local calendar date and
time. It does not provide a way to set the host clock. Request a service table
large enough to include each method before using it.

`audio_open` opens the selected SolarOS playback output, including I2S expansion
outputs, and returns its sample rate and channel count. `audio_write` accepts
signed 16-bit PCM in that format. `audio_close` releases the output. An app can
open and close it to discover the output rate before starting synth audio;
direct PCM playback and synth playback cannot be open together.

Packed INDEX2 frames use four shades of the current display theme. The appended
synth methods accept a bounded block size and required sample rate. Callbacks
render signed 16-bit stereo PCM under a host mutex. The module uses
`synth_lock` and `synth_unlock` when accessing shared synthesizer state; the host
joins synth callbacks on suspend and close. The mutex remains available while
suspended. Failed synth startup leaves video and emulation usable without audio.

The memory service provides PSRAM allocation and optional `alloc_internal` for
hot state or caches. Internal allocation preserves the OS reserve and returns
NULL when unavailable; modules can omit caches or fall back to PSRAM. The host
also provides the current RTC register values, board-button source identity,
and presenter statistics. Standard compiler helpers and `setjmp`/`longjmp`
are exported for synchronous interpreter error handling.

## Emulator apps

See [Apple II Plus](app.apple2.md) for bundled ROMs, NIB disk requirements, controls, and
explicit disk saves. See [Game Boy](app.gameboy.md) for cartridge compatibility,
controls, and automatic cartridge RAM saves. See [Tiny386](app.tiny386.md) for
bootable PC floppy and IDE images, embedded BIOS, and display requirements.
See [DOOM](app.doom.md) for IWAD requirements and keyboard controls. For example,
with assets present:

```text
apple2 /sdcard/apple2/dos33.nib
gameboy /sdcard/roms/game.gb
tiny386 /sdcard/tiny386/dos.img
```

Apple II disk writes remain in memory until explicitly saved. Session switches
retain unsaved disk changes and each emulator's pause state.

# EPD pixel-budget layer

`src/drivers/epd_pixel_budget.h` provides a controller-independent monochrome
target and remaining-cycle model. It owns no SPI bus, GPIO, display task,
allocation, or waveform. A panel driver supplies storage, maps physical pixel
order to indices, and encodes HOLD/DRIVE_BLACK/DRIVE_WHITE into its controller's
RAM and waveform selections.

Drivers may attach caller-owned `uint32_t[count]` active-index storage before
seeding or retargeting. Budget advancement then visits pending pixels only.
The list compacts after a successful cycle; seeded entries can remain listed
until compaction and must be checked with `epd_pixel_budget_drive()`. Reset
the model before seeding a complete native baseline. Without active storage,
the same API uses a full-map scan.

Drivers may also attach caller-owned `uint16_t[count]` drive-history storage
before targeting. Each successful `advance()` increments only actually driven
pixels, with saturating counters and a peak count since reset. Both sparse and
dense paths produce the same history. Submissions, HOLD pixels, failed waveforms,
and idle time do not add exposure; mid-transition reversals retain earlier
exposure. `reset()` clears the history and peak, while `seed()` alone preserves
them. This costs two additional bytes per pixel when attached and supplies no
optical feedback. Drivers interpret the history using their calibrated profile.

Seed every pixel after a successful native cleanup. A changed target receives
the configured cycle budget. Repeating that target preserves its remaining
budget; reversing it assigns a new budget in the new direction. Call
`epd_pixel_budget_advance()` only after a complete successful waveform.
Pixels with zero remaining cycles return HOLD. A failed waveform leaves
optical state unknown: reset/reseed from a native cleanup before continuing.

The common tuning parser handles `drive-cycles`, `pulse-frames`,
`waveform-fr`, `cleanup-every`, and `cleanup-policy=<adaptive,fixed>`.
Unknown keys return `ESP_ERR_NOT_FOUND`;
invalid values leave configuration unchanged. `waveform-fr` is a profile's raw
frame-rate code, not a portable frequency. Profiles may impose narrower limits.

`epd_scan_worker.h` supplies an optional persistent task with a latest-frame
mailbox. Submitting copies the frame without waiting for panel I/O; repeated
queued submissions coalesce. The adapter callback performs at most one drive
cycle and reports whether work remains. The worker continues until settling
finishes and accepts retargeting between cycles. The requested minimum interval
paces distinct target snapshots; pending settling drives run as fast as the
panel callback permits. Targets queued before their image slot coalesce while
the preceding snapshot continues settling. Identical snapshots neither consume
an image slot nor refill budgets. Hardware time can exceed the interval.
Statistics separate successfully processed distinct snapshots (including native
baselines) and their start-to-start time from completed drives and drive timing.
The adapter owns bus serialization, power transitions, and failure recovery.
Disable the worker during native operation and join it before releasing panel
resources. The mailbox lock must never be held while acquiring a panel lock.

The dual-SSD1683 5.79-inch adapter is the first consumer. It programs both
controllers through a panel profile supplying physical geometry, RAM-plane size,
native-target transfer, drive-mask transfer, and waveform setup callbacks.
Budget allocation, retargeting, cleanup, scan scheduling, and failure recovery
use that profile rather than assuming the dual panel's dimensions. Retargeting
ignores non-visible bits in U8g2's final padded tile row.
The dual profile programs both
controllers, replaces both RAM planes whenever the active drive mask changes,
reuses them between identical cycles with RAM ping-pong disabled, holds seam padding
neutral, and serializes tuning with panel operations. The adapter uses its scan
worker by default in pulse mode; synchronous completion remains selectable.
The scan worker consumes one cycle per callback, retargets only changed image bits,
and keeps a copied target
independent of the renderer's buffer. The mailbox is separate from the panel
lock, so BUSY waits do not block submissions. DRAW_TILE is a no-op; REFRESH
submits the completed buffer in asynchronous mode. The model supplies no
optical feedback or grayscale state. Adaptive cleanup uses the peak successful
drives per pixel, with a limit of `cleanup-every * drive-cycles`. It also bounds
changed-image age at `4 * cleanup-every`; fixed policy uses `cleanup-every` alone.
Image age counts completed changed presentations in synchronous mode and changed
targets consumed in asynchronous mode. Identical targets do not age it; continued
driving does accumulate pixel history. The asynchronous callback reports more
work when either limit is reached, even if all pixels just finished settling,
so the worker performs the cleanup without a new presentation. The next callback
establishes a native baseline from its newest target, even with old budgets still
pending. Reset history only after successful native cleanup. The fourfold age
bound is a configurable-baseline heuristic to limit continuous animation and
neutral-waveform exposure; it is not a measured ghosting threshold.
The first custom activation clears native image bits from all four RAM planes.
Later writes cover the union of the current and preceding active bounds, so
old drive bits become neutral before another activation. This restricts SPI
traffic, not the controller's physical gate scan.
The Waveshare V2/revision-2.2 4.2-inch profile supplies a single-controller
400x300 adapter with X-fast, increasing-Y row-major RAM windows. Its first mask
replaces both full RAM planes; later writes cover the union of active and prior
bounds. Its native target transfer mirrors the image into both planes.
The pulse loader explicitly selects both-plane B/W LUT mapping (0x21=00/00),
disables RAM ping-pong, and uses VCOM 0x30, VGH 0x17, source levels 41/A8/32,
and booster strengths 8B/9C/A4 from
[Waveshare's EPD_4in2_V2 custom-LUT setup](https://github.com/waveshareteam/e-Paper/blob/master/RaspberryPi_JetsonNano/c/lib/e-Paper/EPD_4in2_V2.c).
SolarOS generates its own differential waveform and uses the SSD1683's documented
two-frame discharge sequence. This profile starts with normal ramp and supply
cycling and leaves native auto refresh as the boot policy. Native operation is
restored by reset before resuming the existing partial/full path.

SSD1677 and UltraChip profiles need their own validated waveform adapters
before enabling the same controls. Matching a controller family alone does
not establish a panel's electrical or optical compatibility.

Waveform register layout follows Solomon Systech's SSD1683 Rev 1.0, sections
6.5–6.7. Dual-controller custom-LUT programming was cross-checked against
Good Display's panel sequence as published in
[Typoena's GDEY0579T93 driver](https://github.com/typoena/typewriter/blob/main/firmware/src/drivers/screen_epd.rs).
SolarOS generates its own short differential waveform; it does not reuse the
other project's full waveform table. Pulse mode explicitly programs the
panel's published source/gate rail and VCOM settings on both controllers,
including booster soft start, and disables RAM ping-pong because it writes
both planes explicitly. Pulse start, completion, and errors use the SolarOS
log ring and can be read with `log show` over a port shell.
The optional fast booster duration is 0x00 (10/10/10 ms) instead of 0x0f
(40/40/10 ms), per SSD1683 section 7 command 0x0c. It retains current/strength
and rail settings. Holding power uses 0xcc and explicitly shuts down via 0x03
when budgets finish. Sleep rejects queued targets, and resume resets/reseeds.
Failed asynchronous pulses disable the worker and recover the newest accepted
snapshot through a native full update. SPI frequency overrides use the
ESP-IDF transaction override, apply only in pulse mode, and restore the board
clock for native operation. Requested period and actual scan statistics are
separate; only a panel test can establish useful optical update cadence.

# Sticky V2 SSD2677 pulse waveform

The SSD2677 adapter uses the same `epd_pixel_budget` and `epd_scan_worker` as
SSD1677. Its RAM protocol and waveform transport are controller-specific.

## Transport

Command `0x20` accepts a 535-byte RAM waveform: seven parameter bytes followed
by six 88-byte tables. Each table contains eight 11-byte groups. The tables
describe VCOM, pixel codes `00`, `01`, `10`, `11`, and power mode. The reference
for this upload path is [FreeInk's SSD2677 implementation](https://github.com/Free-Ink/freeink-sdk/blob/f1e1d2be2594cc4750d4f9114d13c80845691775/libs/display/FreeInkDisplay/src/driver/Ssd2677Driver.cpp)
and its [waveform tables](https://github.com/Free-Ink/freeink-sdk/blob/f1e1d2be2594cc4750d4f9114d13c80845691775/libs/display/FreeInkDisplay/src/lut/Ssd2677Luts.h).
Attribution is in `licenses/freeink-ssd2677-notice.txt`.

`epd_ssd2677_waveform.c` constructs a bounded short profile from those transport
parameters. Only the first group is enabled. Codes `00` and `11` follow VCOM;
`01` drives white and `10` drives black. The packed image is therefore a pair
of drive masks, not the previous and next optical images. Inversion swaps the
two drive selectors while preserving HOLD.

The calibrated group counter encodes active-frame bits 0..5 at transport bits
4, 7, 6, 1, 0, 3, respectively, with inverted polarity. Thus counts 1, 2, 4,
8, 16, and 32 encode as `EF`, `7F`, `BF`, `FD`, `FE`, and `F7`. The selected
short phase uses `EF` in group bytes 9 and 10. The other timing bytes remain
`FF`; unused groups remain disabled. This is the calibrated pulse profile,
not a general-purpose description of every waveform field.

Command `0x30` selects the documented scan rate: codes 1..5 mean 25, 50, 65,
75, and 85 Hz. On the tested Sticky V2, short profiles at 85 Hz take approximately
`(active_frames + 1) * 12 ms`, excluding image transfer and power-on.
One-frame pulses moved both black-to-white and white-to-black bands gradually,
while matching VCOM kept unchanged bands stable. Repeated pulses settle the
target; an intermediate optical gray is not a completed monochrome target.

## State and recovery

The adapter resets the controller before switching from a latched factory
waveform to RAM waveform operation. PSR retains `LUT_EN=0`, so the controller
uses this panel's OTP analog settings for power-on and refresh. The pulse path
does not program OTP or replace the source/gate voltage registers.

New target pixels receive a bounded drive budget. A successful waveform
consumes one cycle; failed transfers or BUSY timeouts consume none. Unchanged
masks reuse panel RAM, and the worker continues pending settling without a
new application frame. New submissions replace queued targets.

After initializing the visible RAM with a complete neutral mask, pulse updates
use command `0x83` to address an aligned rectangle. The transferred rectangle
includes both the active pixels and the preceding mask, clearing pixels that
have finished settling to HOLD. Coordinates account for the V2 reversed gate
rows. Controller reset invalidates the remembered window before the next full
mask initialization. Native factory refreshes retain full-frame transfers.
Pixel budgets follow native tile-byte order, keeping adjacent pixel/history
entries together and allowing drive-mask generation without per-pixel coordinate
division.

Cleanup, relax recovery, final power-state images, and resumed optical
baselines use factory full refresh. A subsequent pulse resets and reloads the
short RAM waveform. Mode changes serialize with the panel and stop/join the
worker outside the panel lock before releasing its snapshots.

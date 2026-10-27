"""Foreground SolarOS probe for asynchronous pulse scanning.

Configure refresh=pulse and pulse-scan=async from telnet before launch.
This schedules image targets, not physical completions. Read actual cadence
from display mode/log show and inspect text, contrast, trails, and the seam.
"""
from solaros import gfx, time

FRAME_MS = 67
FRAMES = 180
FINAL_HOLD_MS = 60000

gfx.begin()
try:
    width, height = gfx.size()
    patch = min(68, height // 3)

    def draw(x, step):
        gfx.clear(gfx.WHITE)
        gfx.color(gfx.BLACK)
        gfx.font(gfx.FONT_MONO)
        gfx.text(8, 24, "67 ms targets - inspect contrast, dithering, and trails")
        gfx.text(8, 44, "step " + str(step))
        gfx.rect(2, 2, width - 4, height - 4)
        # Exercise the normal gfx dithering path. Static shades should hold
        # while the moving black/gray patch crosses both controller halves.
        for shade in range(gfx.GRAY_MAX + 1):
            left = 8 + shade * (width - 16) // (gfx.GRAY_MAX + 1)
            right = 8 + (shade + 1) * (width - 16) // (gfx.GRAY_MAX + 1)
            gfx.color(gfx.gray(shade))
            gfx.fill_rect(left, 60, right - left, 36)
        gfx.color(gfx.BLACK)
        gfx.fill_rect(x, height // 2, patch, patch)
        gfx.color(gfx.gray(gfx.GRAY_MAX // 2))
        gfx.fill_rect(x + patch // 2, height // 2, patch - patch // 2, patch)
        gfx.present()

    draw(8, 0)
    time.sleep_ms(5000)  # Initial native baseline and mask initialization.
    due = time.uptime_ms()
    for frame in range(FRAMES):
        draw(8 + (frame * 8) % max(1, width - patch - 16), frame // 15)
        due += FRAME_MS
        delay = due - time.uptime_ms()
        if delay > 0:
            time.sleep_ms(delay)
    print("queued", FRAMES, "targets; inspect driver scan timings")
    # Leave the final target visible long enough for a human to inspect erasure.
    # Ending gfx ownership lets the foreground screen replace this result.
    time.sleep_ms(FINAL_HOLD_MS)
finally:
    gfx.end()

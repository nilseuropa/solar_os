#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_gfx.h"
#include "u8g2.h"

/*
 * Read-only access to the live display framebuffer for the remote
 * (web screen share) job. SolarOS renders everything -- terminal and
 * graphics apps alike -- into u8g2's 1bpp full framebuffer in RAM, so
 * a "screenshot" is just a copy of that buffer; no display-controller
 * readback is needed or possible (several boards have no MISO wired).
 *
 * The snapshot is taken without locking against the render loop: at
 * ~10KB per copy a torn frame is a rare, purely cosmetic glitch on
 * the next refresh, which is a fine trade for not stalling rendering.
 */

void solar_os_remote_screen_attach(u8g2_t *u8g2, const solar_os_gfx_t *gfx);
bool solar_os_remote_screen_available(void);

/*
 * Logical (rotation-applied) dimensions of the screen right now --
 * tracks setterm orientation changes automatically, since the active
 * u8g2 rotation callback is consulted on every call.
 */
esp_err_t solar_os_remote_screen_get_size(uint16_t *width, uint16_t *height);

/*
 * Copies the current frame as packed 1bpp scanlines in LOGICAL
 * orientation: row 0 = top of what the physical screen shows, MSB of
 * each byte = leftmost pixel, bit set = white (polarity from the gfx
 * layer's black_is_one is already applied). Row stride is
 * (width + 7) / 8 bytes with no extra padding. out_size must be at
 * least stride * height bytes.
 */
esp_err_t solar_os_remote_screen_snapshot(uint8_t *out,
                                          size_t out_size,
                                          uint16_t *width,
                                          uint16_t *height);

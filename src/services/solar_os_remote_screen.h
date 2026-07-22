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
 * The main loop writes into that buffer and the httpd task reads it
 * concurrently, so both sides share one mutex: the main loop holds it
 * for the span of a tick's drawing (see solar_os_remote_screen_render_lock,
 * called from main.c around dispatch_app_tick/draw_terminal_if_needed/
 * draw_session_overlay_if_needed) and solar_os_remote_screen_snapshot()
 * holds it while copying. Each side gets a clean, fully-drawn buffer
 * or a fully-read one, never a mix -- keeping each snapshot to just
 * the rows a caller needs (see below) keeps the lock's hold time, and
 * so the main loop's worst-case stall, short.
 */

void solar_os_remote_screen_attach(u8g2_t *u8g2, const solar_os_gfx_t *gfx);
bool solar_os_remote_screen_available(void);

/*
 * Render-side critical section, held by the main loop for the
 * duration of a tick's drawing. solar_os_remote_screen_snapshot()
 * takes the same lock internally -- callers of these two functions
 * must not also be inside a snapshot call, and vice versa. Both are
 * no-ops before the first solar_os_remote_screen_attach().
 */
void solar_os_remote_screen_render_lock(void);
void solar_os_remote_screen_render_unlock(void);

/*
 * Logical (rotation-applied) dimensions of the screen right now --
 * tracks setterm orientation changes automatically, since the active
 * u8g2 rotation callback is consulted on every call.
 */
esp_err_t solar_os_remote_screen_get_size(uint16_t *width, uint16_t *height);

/*
 * Copies rows [y0, y1) of the current frame as packed 1bpp scanlines
 * in LOGICAL orientation: output row 0 = logical row y0, MSB of each
 * byte = leftmost pixel, bit set = white (polarity from the gfx
 * layer's black_is_one is already applied). Row stride is
 * (width + 7) / 8 bytes with no extra padding. out_size must be at
 * least stride * (y1 - y0) bytes. y1 must be <= the logical height
 * from solar_os_remote_screen_get_size(); pass y0=0, y1=height for
 * the whole frame.
 */
esp_err_t solar_os_remote_screen_snapshot(uint8_t *out,
                                          size_t out_size,
                                          uint16_t y0,
                                          uint16_t y1,
                                          uint16_t *width,
                                          uint16_t *height);

#pragma once

/* BEGIN saves the image and suspends ordinary presentation. BLACK/WHITE
 * perform synchronous native full refreshes. END restores the saved image
 * and prior refresh policy, including after a failed recovery frame.
 */
typedef enum {
    SOLAR_OS_DISPLAY_RELAX_BEGIN,
    SOLAR_OS_DISPLAY_RELAX_BLACK,
    SOLAR_OS_DISPLAY_RELAX_WHITE,
    SOLAR_OS_DISPLAY_RELAX_END,
} solar_os_display_relax_phase_t;

#include "solar_os_board_pointer.h"

#include <string.h>

#include "ft6336.h"
#include "solar_os_board.h"

esp_err_t solar_os_board_pointer_init(void)
{
    return ft6336_init();
}

esp_err_t solar_os_board_pointer_read(solar_os_board_pointer_sample_t *sample)
{
    if (sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ft6336_sample_t raw;
    const esp_err_t err = ft6336_read(&raw);
    if (err != ESP_OK) {
        return err;
    }

    memset(sample, 0, sizeof(*sample));
    sample->pressed = raw.touched;
    sample->pointer_id = raw.id;
    if (!raw.touched) {
        return ESP_OK;
    }

    /* Controller coordinates are portrait; SolarOS uses landscape rotation 1. */
    if (raw.x >= SOLAR_OS_BOARD_DISPLAY_NATIVE_WIDTH ||
        raw.y >= SOLAR_OS_BOARD_DISPLAY_NATIVE_HEIGHT) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    sample->x = raw.y;
    sample->y = (SOLAR_OS_BOARD_DISPLAY_NATIVE_WIDTH - 1U) - raw.x;
    return ESP_OK;
}

void solar_os_board_pointer_deinit(void)
{
    ft6336_deinit();
}

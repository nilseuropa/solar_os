#include "solar_os_pointer.h"

#include <stdbool.h>
#include <string.h>

#include "solar_os_board_caps.h"
#include "solar_os_display.h"
#include "solar_os_input.h"

#if SOLAR_OS_BOARD_HAS_POINTER
#include "solar_os_board_pointer.h"
#endif

static solar_os_input_source_t pointer_source = SOLAR_OS_INPUT_SOURCE_INVALID;
static bool pointer_pressed;
static uint8_t pointer_id;
static int16_t pointer_x;
static int16_t pointer_y;

esp_err_t solar_os_pointer_init(void)
{
#if !SOLAR_OS_BOARD_HAS_POINTER
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (pointer_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        return ESP_OK;
    }
    esp_err_t err = solar_os_board_pointer_init();
    if (err != ESP_OK) {
        return err;
    }
    err = solar_os_input_source_open("touch0", &pointer_source);
    if (err != ESP_OK) {
        solar_os_board_pointer_deinit();
    }
    return err;
#endif
}

void solar_os_pointer_poll(void)
{
#if SOLAR_OS_BOARD_HAS_POINTER
    if (pointer_source == SOLAR_OS_INPUT_SOURCE_INVALID) {
        return;
    }
    solar_os_board_pointer_sample_t sample;
    if (solar_os_board_pointer_read(&sample) != ESP_OK) {
        return;
    }

    solar_os_input_pointer_action_t action;
    if (sample.pressed && !pointer_pressed) {
        action = SOLAR_OS_INPUT_POINTER_PRESS;
    } else if (!sample.pressed && pointer_pressed) {
        action = SOLAR_OS_INPUT_POINTER_RELEASE;
    } else if (sample.pressed &&
               (sample.x != (uint16_t)pointer_x || sample.y != (uint16_t)pointer_y)) {
        action = SOLAR_OS_INPUT_POINTER_MOVE;
    } else {
        return;
    }

    const int16_t next_x = sample.pressed ? (int16_t)sample.x : pointer_x;
    const int16_t next_y = sample.pressed ? (int16_t)sample.y : pointer_y;
    solar_os_input_pointer_event_t event = {
        .pointer_id = sample.pressed ? sample.pointer_id : pointer_id,
        .buttons = sample.pressed ? SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY : 0,
        .mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE,
        .action = action,
        .x = next_x,
        .y = next_y,
        .delta_x = (int16_t)(next_x - pointer_x),
        .delta_y = (int16_t)(next_y - pointer_y),
    };
    strlcpy(event.target, SOLAR_OS_DISPLAY_PRIMARY_TARGET, sizeof(event.target));
    if (solar_os_input_write_pointer(pointer_source, &event) == ESP_OK) {
        pointer_pressed = sample.pressed;
        pointer_id = event.pointer_id;
        pointer_x = next_x;
        pointer_y = next_y;
    }
#endif
}

void solar_os_pointer_deinit(void)
{
#if SOLAR_OS_BOARD_HAS_POINTER
    if (pointer_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        solar_os_input_source_close(pointer_source);
        pointer_source = SOLAR_OS_INPUT_SOURCE_INVALID;
    }
    solar_os_board_pointer_deinit();
    pointer_pressed = false;
#endif
}

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvs.h"
#include "solar_os_input.h"
#include "solar_os_keys.h"
#include "solar_os_memory.h"

static int64_t now_us;
static size_t pointer_queue_allocations;
static size_t pointer_queue_frees;

void *solar_os_memory_calloc(size_t count,
                             size_t size,
                             solar_os_memory_class_t memory_class,
                             const char *tag)
{
    assert(count == 32);
    assert(size == sizeof(solar_os_input_pointer_event_t));
    assert(memory_class == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED);
    assert(strcmp(tag, "input-pointer") == 0);
    pointer_queue_allocations++;
    return calloc(count, size);
}

void solar_os_memory_free(void *ptr)
{
    if (ptr != NULL) {
        pointer_queue_frees++;
        free(ptr);
    }
}

int64_t esp_timer_get_time(void)
{
    return now_us;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t length = strlen(src);
    if (size > 0) {
        const size_t copy = length < size - 1U ? length : size - 1U;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return length;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    (void)name;
    if (mode == NVS_READONLY) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *value)
{
    (void)handle;
    (void)key;
    (void)value;
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value)
{
    (void)handle;
    (void)key;
    (void)value;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    (void)handle;
}

static solar_os_input_key_event_t read_one_event(void)
{
    solar_os_input_key_event_t event = {0};
    assert(solar_os_input_read_events(&event, 1) == 1);
    return event;
}

int main(void)
{
    assert(solar_os_input_init() == ESP_OK);
    assert(solar_os_input_translate_hid_usage(0x04, 0, false) == 'a');
    assert(solar_os_input_translate_hid_usage(0x04,
                                              SOLAR_OS_INPUT_MOD_LEFT_SHIFT,
                                              false) == 'A');
    assert(solar_os_input_translate_hid_usage(0x19,
                                              SOLAR_OS_INPUT_MOD_LEFT_CTRL,
                                              false) == 0x16);
    assert(solar_os_input_translate_hid_usage(0x30,
                                              SOLAR_OS_INPUT_MOD_LEFT_CTRL,
                                              false) == SOLAR_OS_KEY_APP_EXIT);
    assert(solar_os_input_set_keyboard_layout(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE) == ESP_OK);
    assert(solar_os_input_translate_hid_usage(0x1c, 0, false) == 'z');
    assert(solar_os_input_translate_hid_usage(0x2b,
                                              SOLAR_OS_INPUT_MOD_RIGHT_ALT,
                                              false) == '\t');
    assert(solar_os_input_set_keyboard_layout(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_US) == ESP_OK);

    solar_os_input_source_t keyboard = SOLAR_OS_INPUT_SOURCE_INVALID;
    solar_os_input_source_t buttons = SOLAR_OS_INPUT_SOURCE_INVALID;
    assert(solar_os_input_keyboard_source_open("keyboard", true, &keyboard) == ESP_OK);
    assert(solar_os_input_key_source_open("buttons",
                                          SOLAR_OS_INPUT_SOURCE_BUTTONS,
                                          &buttons) == ESP_OK);
    assert(keyboard != buttons);
    assert(solar_os_input_keyboard_count() == 1);
    assert(pointer_queue_allocations == 0);

    assert(solar_os_input_source_count() == 2);
    solar_os_input_source_info_t source_info = {0};
    assert(solar_os_input_source_get(0, &source_info));
    assert(source_info.source == keyboard);
    assert(strcmp(source_info.name, "keyboard") == 0);
    assert(source_info.source_class == SOLAR_OS_INPUT_SOURCE_KEYBOARD);
    assert(source_info.capabilities == SOLAR_OS_INPUT_CAP_KEY_EVENTS);
    assert(source_info.ready);
    assert(strcmp(solar_os_input_source_class_name(source_info.source_class),
                  "keyboard") == 0);
    assert(solar_os_input_source_get(1, &source_info));
    assert(source_info.source_class == SOLAR_OS_INPUT_SOURCE_BUTTONS);
    assert(!solar_os_input_source_get(2, &source_info));

    solar_os_input_source_t keyboard_status = SOLAR_OS_INPUT_SOURCE_INVALID;
    assert(solar_os_input_keyboard_source_open("keyboard-status",
                                               false,
                                               &keyboard_status) == ESP_OK);
    assert(solar_os_input_keyboard_count() == 1);
    assert(solar_os_input_keyboard_source_set_ready(keyboard_status, true) == ESP_OK);
    assert(solar_os_input_keyboard_count() == 2);
    assert(solar_os_input_keyboard_source_set_ready(buttons, true) == ESP_ERR_INVALID_ARG);

    assert(solar_os_input_write_key(keyboard,
                                    0x1d,
                                    0x1d,
                                    'z',
                                    0,
                                    SOLAR_OS_INPUT_KEY_PRESS) == ESP_OK);
    solar_os_input_key_event_t pressed[2];
    assert(solar_os_input_get_pressed(pressed, 2) == 1);
    assert(pressed[0].source == keyboard);
    assert(pressed[0].usage == 0x1d);
    assert(pressed[0].key == 'z');

    solar_os_input_key_event_t event = read_one_event();
    assert(event.action == SOLAR_OS_INPUT_KEY_PRESS);
    assert(event.physical_key == 0x1d);

    assert(solar_os_input_write_key(keyboard,
                                    0x1d,
                                    0x1d,
                                    'z',
                                    0,
                                    SOLAR_OS_INPUT_KEY_PRESS) == ESP_OK);
    assert(solar_os_input_read_events(&event, 1) == 0);

    now_us = 449000;
    assert(solar_os_input_read_events(&event, 1) == 0);
    now_us = 450000;
    event = read_one_event();
    assert(event.action == SOLAR_OS_INPUT_KEY_REPEAT);
    assert(event.key == 'z');

    assert(solar_os_input_write_key(keyboard,
                                    0x1d,
                                    0x1d,
                                    'z',
                                    0,
                                    SOLAR_OS_INPUT_KEY_RELEASE) == ESP_OK);
    assert(solar_os_input_get_pressed(pressed, 2) == 0);
    event = read_one_event();
    assert(event.action == SOLAR_OS_INPUT_KEY_RELEASE);

    now_us = 500000;
    assert(solar_os_input_write_key(keyboard,
                                    0x28,
                                    0x28,
                                    SOLAR_OS_KEY_ENTER,
                                    0,
                                    SOLAR_OS_INPUT_KEY_PRESS) == ESP_OK);
    event = read_one_event();
    assert(event.action == SOLAR_OS_INPUT_KEY_PRESS);
    assert(event.key == SOLAR_OS_KEY_ENTER);
    now_us = 1000000;
    assert(solar_os_input_read_events(&event, 1) == 0);
    assert(solar_os_input_write_key(keyboard,
                                    0x28,
                                    0x28,
                                    SOLAR_OS_KEY_ENTER,
                                    0,
                                    SOLAR_OS_INPUT_KEY_RELEASE) == ESP_OK);
    event = read_one_event();
    assert(event.action == SOLAR_OS_INPUT_KEY_RELEASE);

    now_us = 1100000;
    assert(solar_os_input_write_key(keyboard,
                                    0x2b,
                                    0x2b,
                                    '\t',
                                    0,
                                    SOLAR_OS_INPUT_KEY_PRESS) == ESP_OK);
    event = read_one_event();
    assert(event.action == SOLAR_OS_INPUT_KEY_PRESS);
    assert(event.key == '\t');
    now_us = 2000000;
    assert(solar_os_input_read_events(&event, 1) == 0);
    assert(solar_os_input_write_key(keyboard,
                                    0x2b,
                                    0x2b,
                                    '\t',
                                    0,
                                    SOLAR_OS_INPUT_KEY_RELEASE) == ESP_OK);
    event = read_one_event();
    assert(event.action == SOLAR_OS_INPUT_KEY_RELEASE);

    assert(solar_os_input_write_key(keyboard,
                                    0x2b,
                                    0x2b,
                                    '\t',
                                    SOLAR_OS_INPUT_MOD_LEFT_ALT,
                                    SOLAR_OS_INPUT_KEY_PRESS) == ESP_OK);
    assert(solar_os_input_write_char(buttons, 'b') == ESP_OK);
    char chars[3] = {0};
    assert(solar_os_input_read_source_chars(keyboard, chars, sizeof(chars)) == 2);
    assert((uint8_t)chars[0] == SOLAR_OS_KEY_ALT_PREFIX);
    assert(chars[1] == '\t');
    assert(solar_os_input_read_source_chars(buttons, chars, sizeof(chars)) == 1);
    assert(chars[0] == 'b');

    solar_os_input_pointer_event_t pointer = {
        .pointer_id = 2,
        .buttons = SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY,
        .mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE,
        .action = SOLAR_OS_INPUT_POINTER_PRESS,
        .x = 123,
        .y = 45,
        .delta_x = 3,
        .delta_y = -2,
        .target = "display0",
    };
    assert(solar_os_input_write_pointer(buttons, &pointer) == ESP_ERR_INVALID_STATE);
    assert(pointer_queue_allocations == 0);
    solar_os_input_source_t pointer_source = SOLAR_OS_INPUT_SOURCE_INVALID;
    solar_os_input_source_t pointer_source_2 = SOLAR_OS_INPUT_SOURCE_INVALID;
    assert(solar_os_input_touch_source_open("touch0", &pointer_source) == ESP_OK);
    assert(pointer_queue_allocations == 1);
    assert(solar_os_input_mouse_source_open("mouse0", &pointer_source_2) == ESP_OK);
    assert(pointer_queue_allocations == 1);
    assert(solar_os_input_write_pointer(pointer_source_2, &pointer) == ESP_ERR_INVALID_STATE);
    assert(solar_os_input_write_pointer(pointer_source, &pointer) == ESP_OK);
    solar_os_input_pointer_event_t pointer_read = {0};
    assert(solar_os_input_read_pointer_events(&pointer_read, 1) == 1);
    assert(pointer_read.source == pointer_source);
    assert(pointer_read.pointer_id == 2);
    assert(pointer_read.action == SOLAR_OS_INPUT_POINTER_PRESS);
    assert(pointer_read.x == 123 && pointer_read.y == 45);
    assert(strcmp(pointer_read.target, "display0") == 0);
    pointer.mode = SOLAR_OS_INPUT_POINTER_RELATIVE;
    assert(solar_os_input_write_pointer(pointer_source, &pointer) == ESP_ERR_INVALID_STATE);
    assert(solar_os_input_write_pointer(pointer_source_2, &pointer) == ESP_OK);
    assert(solar_os_input_read_pointer_events(&pointer_read, 1) == 1);
    assert(pointer_read.source == pointer_source_2);
    pointer.mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE;
    assert(solar_os_input_write_pointer(pointer_source, &pointer) == ESP_OK);
    solar_os_input_source_close(pointer_source);
    assert(solar_os_input_read_pointer_events(&pointer_read, 1) == 0);
    assert(pointer_queue_frees == 0);
    solar_os_input_source_close(pointer_source_2);
    assert(pointer_queue_frees == 1);
    solar_os_input_source_close(buttons);

    assert(solar_os_input_get_pressed(pressed, 2) == 1);
    solar_os_input_source_close(keyboard);
    assert(solar_os_input_get_pressed(pressed, 2) == 0);
    assert(solar_os_input_keyboard_count() == 1);
    solar_os_input_source_close(keyboard_status);
    assert(solar_os_input_keyboard_count() == 0);

    assert(solar_os_input_set_repeat(20, 300) == ESP_OK);
    uint16_t rate = 0;
    uint16_t delay = 0;
    solar_os_input_get_repeat(&rate, &delay);
    assert(rate == 20);
    assert(delay == 300);

    puts("input_test: ok");
    return 0;
}

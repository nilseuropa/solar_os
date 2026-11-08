#include "solar_os_usb_hid_input.h"
#include "solar_os_key_codes.h"

#include <string.h>

static bool has_key(const uint8_t *keys, size_t count, uint8_t key)
{
    return key != 0 && memchr(keys, key, count) != NULL;
}

esp_err_t solar_os_usb_hid_input_open(solar_os_usb_hid_input_t *input,
                                      const char *name, uint8_t protocol)
{
    if (input == NULL || name == NULL) return ESP_ERR_INVALID_ARG;
    memset(input, 0, sizeof(*input));
    input->protocol = protocol;
    if (protocol == SOLAR_OS_USB_HID_KEYBOARD) {
        return solar_os_input_keyboard_source_open(name, true, &input->source);
    }
    if (protocol == SOLAR_OS_USB_HID_MOUSE) {
        return solar_os_input_mouse_source_open(name, &input->source);
    }
    if (protocol == SOLAR_OS_USB_HID_JOYSTICK) {
        return solar_os_input_source_open_typed(name, SOLAR_OS_INPUT_SOURCE_JOYSTICK,
            SOLAR_OS_INPUT_CAP_AXIS_EVENTS | SOLAR_OS_INPUT_CAP_KEY_EVENTS, true, &input->source);
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static void joystick_neutral_axes(solar_os_usb_hid_input_t *input)
{
    for (unsigned i = 0; i < SOLAR_OS_USB_HID_JOYSTICK_AXIS_MAX; i++) {
        if ((input->axes_valid & (1U << i)) && input->axes[i] != 0) {
            const solar_os_input_axis_event_t event = {
                .axis = (solar_os_input_axis_t)i, .value = 0, .delta = -(int32_t)input->axes[i],
            };
            (void)solar_os_input_write_axis(input->source, &event);
        }
    }
    input->axes_valid = 0;
    memset(input->axes, 0, sizeof(input->axes));
}

void solar_os_usb_hid_input_reset(solar_os_usb_hid_input_t *input)
{
    if (input == NULL || input->source == SOLAR_OS_INPUT_SOURCE_INVALID) return;
    if (input->protocol == SOLAR_OS_USB_HID_JOYSTICK) joystick_neutral_axes(input);
    solar_os_input_source_release_all(input->source);
    if (input->buttons != 0) {
        const solar_os_input_pointer_event_t event = {
            .mode = SOLAR_OS_INPUT_POINTER_RELATIVE,
            .action = SOLAR_OS_INPUT_POINTER_RELEASE,
        };
        (void)solar_os_input_write_pointer(input->source, &event);
    }
    input->buttons = 0;
    input->joystick_buttons = 0;
    input->directions = 0;
    input->modifiers = 0;
    memset(input->keys, 0, sizeof(input->keys));
    memset(input->chars, 0, sizeof(input->chars));
}

void solar_os_usb_hid_input_close(solar_os_usb_hid_input_t *input)
{
    if (input == NULL) return;
    /* source_close retains keyboard releases even when its queue is full. */
    if (input->protocol == SOLAR_OS_USB_HID_MOUSE) solar_os_usb_hid_input_reset(input);
    if (input->protocol == SOLAR_OS_USB_HID_JOYSTICK) joystick_neutral_axes(input);
    solar_os_input_source_close(input->source);
    memset(input, 0, sizeof(*input));
}

static esp_err_t joystick_report(solar_os_usb_hid_input_t *input, const uint8_t *data, size_t length)
{
    solar_os_usb_hid_joystick_sample_t sample;
    esp_err_t ret = solar_os_usb_hid_joystick_decode(&input->joystick_layout, data, length, &sample);
    if (ret == ESP_ERR_NOT_FOUND) return ESP_OK;
    if (ret != ESP_OK) return ret;
    const uint16_t buttons = (input->joystick_buttons & ~sample.buttons_mask) | sample.buttons;
    const uint8_t directions = (input->directions & ~sample.directions_mask) | sample.directions;
    /* Release old controls before pressing new ones, including on diagonals. */
    for (unsigned pressed = 0; pressed < 2; pressed++) {
        for (unsigned i = 0; i < SOLAR_OS_USB_HID_JOYSTICK_BUTTON_MAX; i++) {
            const uint16_t mask = 1U << i;
            if (!((buttons ^ input->joystick_buttons) & mask) || !!(buttons & mask) != pressed) continue;
            ret = solar_os_input_write_key(input->source, (uint16_t)(i + 1), 0, 0, 0,
                pressed ? SOLAR_OS_INPUT_KEY_PRESS : SOLAR_OS_INPUT_KEY_RELEASE);
            if (ret != ESP_OK) return ret;
            input->joystick_buttons = (input->joystick_buttons & ~mask) | (buttons & mask);
        }
        static const uint8_t keys[] = {SOLAR_OS_KEY_UP, SOLAR_OS_KEY_DOWN, SOLAR_OS_KEY_LEFT, SOLAR_OS_KEY_RIGHT};
        static const uint16_t usages[] = {0x52, 0x51, 0x50, 0x4f};
        for (unsigned i = 0; i < 4; i++) {
            const uint8_t mask = 1U << i;
            if (!((directions ^ input->directions) & mask) || !!(directions & mask) != pressed) continue;
            ret = solar_os_input_write_key(input->source, SOLAR_OS_USB_HID_HAT_PHYSICAL_BASE + i,
                usages[i], keys[i], 0, pressed ? SOLAR_OS_INPUT_KEY_PRESS : SOLAR_OS_INPUT_KEY_RELEASE);
            if (ret != ESP_OK) return ret;
            input->directions = (input->directions & ~mask) | (directions & mask);
        }
    }
    for (unsigned i = 0; i < SOLAR_OS_USB_HID_JOYSTICK_AXIS_MAX; i++) {
        const uint8_t mask = 1U << i;
        if (!(sample.axes_mask & mask) || ((input->axes_valid & mask) && input->axes[i] == sample.axes[i])) continue;
        const solar_os_input_axis_event_t event = {
            .axis = (solar_os_input_axis_t)i, .value = sample.axes[i],
            .delta = (int32_t)sample.axes[i] - input->axes[i],
        };
        ret = solar_os_input_write_axis(input->source, &event);
        if (ret != ESP_OK) return ret;
        input->axes[i] = sample.axes[i];
        input->axes_valid |= mask;
    }
    return ESP_OK;
}

static esp_err_t keyboard_report(solar_os_usb_hid_input_t *input,
                                  const uint8_t *data, size_t length)
{
    if (length < 8) return ESP_ERR_INVALID_SIZE;
    const uint8_t modifiers = data[0];
    const uint8_t *keys = data + 2;
    /* ErrorRollOver, POSTFail and ErrorUndefined are not key presses. */
    for (size_t i = 0; i < 6; i++) {
        if (keys[i] >= 1 && keys[i] <= 3) return ESP_ERR_INVALID_RESPONSE;
    }
    esp_err_t ret;
    for (uint8_t bit = 0; bit < 8; bit++) {
        const uint8_t mask = 1U << bit;
        if (((input->modifiers ^ modifiers) & mask) == 0) continue;
        const uint16_t usage = 0xe0U + bit;
        ret = solar_os_input_write_key(input->source, usage, usage, 0, modifiers,
            (modifiers & mask) ? SOLAR_OS_INPUT_KEY_PRESS : SOLAR_OS_INPUT_KEY_RELEASE);
        if (ret != ESP_OK) return ret;
    }
    for (size_t i = 0; i < 6; i++) {
        if (input->keys[i] == 0 || has_key(input->keys, i, input->keys[i]) ||
            has_key(keys, 6, input->keys[i])) continue;
        ret = solar_os_input_write_key(input->source, input->keys[i], input->keys[i],
            input->chars[i], modifiers, SOLAR_OS_INPUT_KEY_RELEASE);
        if (ret != ESP_OK) return ret;
    }
    uint8_t chars[6] = {0};
    if (has_key(keys, 6, 0x39) && !has_key(input->keys, 6, 0x39)) {
        input->caps_lock = !input->caps_lock;
    }
    for (size_t i = 0; i < 6; i++) {
        const uint8_t key = keys[i];
        if (key == 0 || has_key(keys, i, key)) continue;
        if (has_key(input->keys, 6, key)) {
            for (size_t j = 0; j < 6; j++) {
                if (input->keys[j] == key) { chars[i] = input->chars[j]; break; }
            }
            continue;
        }
        chars[i] = solar_os_input_translate_hid_usage(key, modifiers, input->caps_lock);
        ret = solar_os_input_write_key(input->source, key, key, chars[i], modifiers,
                                        SOLAR_OS_INPUT_KEY_PRESS);
        if (ret != ESP_OK) return ret;
    }
    memcpy(input->keys, keys, sizeof(input->keys));
    memcpy(input->chars, chars, sizeof(input->chars));
    input->modifiers = modifiers;
    return ESP_OK;
}

esp_err_t solar_os_usb_hid_input_report(solar_os_usb_hid_input_t *input,
                                        const uint8_t *data, size_t length)
{
    if (input == NULL || data == NULL) return ESP_ERR_INVALID_ARG;
    if (input->source == SOLAR_OS_INPUT_SOURCE_INVALID) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
    const bool previous_caps_lock = input->caps_lock;
    if (input->protocol == SOLAR_OS_USB_HID_JOYSTICK) {
        ret = joystick_report(input, data, length);
    } else if (input->protocol == SOLAR_OS_USB_HID_KEYBOARD) {
        ret = keyboard_report(input, data, length);
    } else if (input->protocol == SOLAR_OS_USB_HID_MOUSE) {
        uint8_t buttons = 0;
        int16_t x = 0, y = 0;
        if (input->mouse_layout.valid) {
            ret = solar_os_usb_hid_mouse_decode(&input->mouse_layout, data, length, &buttons, &x, &y);
            /* Consumer/system reports share the endpoint, but are not pointer releases. */
            if (ret == ESP_ERR_NOT_FOUND) return ESP_OK;
        } else if (length < 3) ret = ESP_ERR_INVALID_SIZE;
        else {
            buttons = data[0] & 7U;
            x = (int8_t)data[1];
            y = (int8_t)data[2];
            ret = ESP_OK;
        }
        if (ret == ESP_OK) {
            if (buttons == input->buttons && x == 0 && y == 0) return ESP_OK;
            solar_os_input_pointer_action_t action = SOLAR_OS_INPUT_POINTER_MOVE;
            if (buttons != input->buttons) action = (buttons & ~input->buttons) ?
                SOLAR_OS_INPUT_POINTER_PRESS : SOLAR_OS_INPUT_POINTER_RELEASE;
            const solar_os_input_pointer_event_t event = {
                .mode = SOLAR_OS_INPUT_POINTER_RELATIVE,
                .action = action,
                .buttons = buttons,
                .delta_x = x,
                .delta_y = y,
            };
            ret = solar_os_input_write_pointer(input->source, &event);
            if (ret == ESP_OK) input->buttons = buttons;
        }
    }
    if (ret != ESP_OK) {
        input->caps_lock = previous_caps_lock;
        solar_os_usb_hid_input_reset(input);
    }
    return ret;
}

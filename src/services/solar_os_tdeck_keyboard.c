#include "solar_os_tdeck_keyboard.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_board.h"
#include "solar_os_buses.h"
#include "solar_os_backlight.h"
#include "solar_os_input.h"
#include "solar_os_input_composition.h"
#include "solar_os_keys.h"
#include "solar_os_task.h"

#define TDECK_COLUMNS 5U
#define TDECK_ROWS 7U
#define TDECK_MODE_RAW 0x03U
#define TDECK_MODE_KEY 0x04U
#define TDECK_POLL_MS 10U
#define TDECK_TASK_STACK 3072U
/* The controller scans 35 cells at 1 ms each, then runs its native Alt+B
 * action. A raw snapshot can expose B before that action has happened. */
#define TDECK_SCAN_SETTLE_MS 50U
#define TDECK_RECONFIGURE_GAP_US 250000

typedef struct { uint8_t normal; uint8_t symbol; } tdeck_key_t;
#ifndef SOLAR_OS_BOARD_TDECK_KEYMAP
/* Default raw-matrix map for LilyGO's T-Deck keyboard controller. Boards can
 * override this when their keycaps or matrix wiring differ. The microphone
 * key supplies Tab normally, digit 0 in the symbol layer, and Ctrl when
 * pressed with Shift (handled by process_matrix). */
#define SOLAR_OS_BOARD_TDECK_KEYMAP \
    {{{0x71,0x23},{0x77,0x31},{0,0},{0x61,0x2a},{0,0},{0x20,0},{0x09,0x30}}, \
     {{0x65,0x32},{0x73,0x34},{0x64,0x35},{0x70,0x40},{0x78,0x38},{0x7a,0x37},{0,0}}, \
     {{0x72,0x33},{0x67,0x2f},{0x74,0x28},{0,0},{0x76,0x3f},{0x63,0x39},{0x66,0x36}}, \
     {{0x75,0x5f},{0x68,0x3a},{0x79,0x29},{SOLAR_OS_KEY_ENTER,0},{0x62,0x21},{0x6e,0x2c},{0x6a,0x3b}}, \
     {{0x6f,0x2b},{0x6c,0x22},{0x69,0x2d},{0x08,0},{0x24,0},{0x6d,0x2e},{0x6b,0x27}}}
#endif

static const tdeck_key_t keymap[TDECK_COLUMNS][TDECK_ROWS] =
    SOLAR_OS_BOARD_TDECK_KEYMAP;

typedef struct {
    bool active;
    volatile bool stop_requested;
    volatile bool worker_done;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t previous[TDECK_COLUMNS];
    bool microphone_ctrl;
    bool microphone_tab_pending;
    uint8_t microphone_tab_modifiers;
    solar_os_input_source_t input_source;
    bool raw_ready;
    bool wait_for_release;
    int64_t last_poll_us;
    TaskHandle_t worker_task;
} tdeck_device_t;

static const char *TAG = "tdeck_keyboard";
static tdeck_device_t device;

static esp_err_t set_backlight(void *user, uint8_t percent)
{
    const tdeck_device_t *dev = user;
    const uint8_t command[] = {0x01U, (uint8_t)((percent * 255U + 50U) / 100U)};
    return solar_os_bus_i2c_transmit(dev->i2c_bus, SOLAR_OS_TDECK_KEYBOARD_ADDRESS,
                                     command, sizeof(command));
}

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t count, char *bus)
{
    bool have_bus = false, have_address = false;
    bus[0] = '\0';
    for (size_t i = 0; i < count; i++) {
        if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS && !have_bus) {
            strlcpy(bus, bindings[i].target, SOLAR_OS_EXPANSION_TARGET_MAX);
            have_bus = true;
        } else if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS &&
                   !have_address && bindings[i].value == SOLAR_OS_TDECK_KEYBOARD_ADDRESS) {
            have_address = true;
        } else return ESP_ERR_INVALID_ARG;
    }
    return have_bus && have_address &&
        solar_os_expansion_find_i2c_bus(bus, NULL, NULL) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static uint16_t key_usage(uint8_t key)
{
    if (key >= 'a' && key <= 'z') return 0x04U + key - 'a';
    switch (key) {
    case SOLAR_OS_KEY_ENTER: return 0x28U;
    case '\b': return 0x2aU;
    case '\t': return 0x2bU;
    case ' ': return 0x2cU;
    default: return SOLAR_OS_INPUT_USAGE_NONE;
    }
}

static void process_matrix(tdeck_device_t *dev,
                           const uint8_t state[TDECK_COLUMNS])
{
    const bool alt = (state[0] & (1U << 4U)) != 0U;
    const bool symbol = (state[0] & (1U << 2U)) != 0U;
    const bool physical_shift = (state[1] & (1U << 6U)) != 0U ||
                       (state[2] & (1U << 3U)) != 0U;
    const bool microphone = (state[0] & (1U << 6U)) != 0U;
    const bool microphone_pressed = microphone &&
        (dev->previous[0] & (1U << 6U)) == 0U;
    const bool microphone_tab = !microphone && dev->microphone_tab_pending;
    if (!microphone) {
        dev->microphone_ctrl = false;
        dev->microphone_tab_pending = false;
    } else if (microphone_pressed) {
        dev->microphone_ctrl = physical_shift && !symbol;
        dev->microphone_tab_pending = !symbol && !dev->microphone_ctrl;
    } else if (dev->microphone_tab_pending && physical_shift && !symbol) {
        /* The controller updates matrix cells sequentially. Mic may arrive
         * before Shift even when both were pressed together. */
        dev->microphone_ctrl = true;
        dev->microphone_tab_pending = false;
        dev->previous[0] &= (uint8_t)~(1U << 6U);
    }
    const bool ctrl = dev->microphone_ctrl;
    const bool shift = physical_shift && !ctrl;
    /* Mic keeps its role until release, so letting go of Shift cannot leak a
     * Tab. Shift selects the Ctrl chord rather than remaining a Shift modifier. */
    uint8_t effective[TDECK_COLUMNS];
    memcpy(effective, state, sizeof(effective));
    if (ctrl) {
        effective[1] &= (uint8_t)~(1U << 6U);
        effective[2] &= (uint8_t)~(1U << 3U);
    }
    solar_os_input_composition_set_modifiers(
        dev->input_source,
        (alt ? SOLAR_OS_INPUT_MOD_LEFT_ALT : 0U) |
        (ctrl ? SOLAR_OS_INPUT_MOD_LEFT_CTRL : 0U) |
        (shift ? SOLAR_OS_INPUT_MOD_LEFT_SHIFT : 0U));
    const uint8_t modifiers = solar_os_input_composition_modifiers();
    if (microphone_pressed && dev->microphone_tab_pending)
        dev->microphone_tab_modifiers = modifiers;
    if (microphone_tab) {
        (void)solar_os_input_write_key_tap(dev->input_source,
            SOLAR_OS_INPUT_PHYSICAL_NONE, SOLAR_OS_INPUT_USAGE_NONE, '\t',
            dev->microphone_tab_modifiers);
    }
    for (size_t col = 0; col < TDECK_COLUMNS; col++) {
        const uint8_t changed = effective[col] ^ dev->previous[col];
        for (size_t row = 0; row < TDECK_ROWS; row++) {
            if ((changed & (1U << row)) == 0U) continue;
            if (col == 0U && row == 6U && dev->microphone_tab_pending) continue;
            const uint16_t physical = (uint16_t)(col * TDECK_ROWS + row + 1U);
            if ((effective[col] & (1U << row)) == 0U) {
                /* The input service retains the press mapping, including its
                 * symbol/chord translation, until this physical release. */
                (void)solar_os_input_write_key(dev->input_source, physical,
                    SOLAR_OS_INPUT_USAGE_NONE, 0U, modifiers, SOLAR_OS_INPUT_KEY_RELEASE);
                continue;
            }
            uint16_t modifier_usage = 0U;
            if (col == 0U && row == 6U && ctrl) modifier_usage = 0xe0U;
            if (col == 0U && row == 4U) modifier_usage = 0xe2U;
            if ((col == 1U && row == 6U) || (col == 2U && row == 3U)) modifier_usage = 0xe1U;
            if (modifier_usage != 0U) {
                (void)solar_os_input_write_key(dev->input_source, physical,
                    modifier_usage, 0U, modifiers, SOLAR_OS_INPUT_KEY_PRESS);
                continue;
            }
            /* Let the controller finish its native toggle before writing the
             * service's absolute level. Otherwise the two toggles race. */
            if (alt && col == 3U && row == 4U) {
                vTaskDelay(pdMS_TO_TICKS(TDECK_SCAN_SETTLE_MS));
                (void)solar_os_backlight_toggle();
                continue;
            }
            uint8_t key = symbol ? keymap[col][row].symbol : keymap[col][row].normal;
            if (key == 0U) continue;
            if (shift && key >= 'a' && key <= 'z') key = (uint8_t)(key - 'a' + 'A');
            const uint8_t composed = solar_os_input_composition_apply_with_modifiers(key, modifiers);
            const uint8_t output = composed == key ?
                solar_os_input_translate_char_key(composed, modifiers) : composed;
            /* Ordinary keys expose canonical physical usages to guests. Sym
             * and board chords are logical replacements; a letter usage would
             * incorrectly turn Sym+E into E instead of the digit 2. */
            const uint16_t usage = !symbol && composed == key ?
                key_usage(keymap[col][row].normal) : SOLAR_OS_INPUT_USAGE_NONE;
            if (composed != 0U) (void)solar_os_input_write_key(dev->input_source,
                physical, usage, output, composed == key ? modifiers : 0U,
                SOLAR_OS_INPUT_KEY_PRESS);
        }
    }
    memcpy(dev->previous, effective, sizeof(dev->previous));
}

static void reset_matrix(tdeck_device_t *dev)
{
    solar_os_input_source_release_all(dev->input_source);
    solar_os_input_composition_set_modifiers(dev->input_source, 0U);
    memset(dev->previous, 0, sizeof(dev->previous));
    dev->microphone_ctrl = false;
    dev->microphone_tab_pending = false;
    dev->raw_ready = false;
    dev->wait_for_release = true;
}

static void poll_matrix(tdeck_device_t *dev)
{
    const int64_t now = esp_timer_get_time();
    if (dev->raw_ready && now - dev->last_poll_us > TDECK_RECONFIGURE_GAP_US)
        reset_matrix(dev);
    dev->last_poll_us = now;

    uint8_t state[TDECK_COLUMNS] = {0};
    if (!dev->raw_ready) {
        /* Sleep or a controller reset can leave character mode or a stale
         * slave TX response behind. Re-select raw mode and drain that response
         * before interpreting any bytes as matrix bits. */
        const uint8_t command = TDECK_MODE_RAW;
        if (solar_os_bus_i2c_transmit(dev->i2c_bus,
                SOLAR_OS_TDECK_KEYBOARD_ADDRESS, &command, 1) != ESP_OK) return;
        vTaskDelay(pdMS_TO_TICKS(TDECK_POLL_MS));
        if (solar_os_bus_i2c_receive(dev->i2c_bus,
                SOLAR_OS_TDECK_KEYBOARD_ADDRESS, state, sizeof(state)) != ESP_OK) return;
        dev->raw_ready = true;
        dev->wait_for_release = true;
        return;
    }
    if (solar_os_bus_i2c_receive(dev->i2c_bus,
            SOLAR_OS_TDECK_KEYBOARD_ADDRESS, state, sizeof(state)) != ESP_OK) {
        reset_matrix(dev);
        return;
    }
    bool released = true;
    for (size_t col = 0; col < TDECK_COLUMNS; col++) {
        /* Only seven row bits exist. Short character-mode responses and
         * interrupted I2C transfers must never become phantom held keys. */
        if ((state[col] & 0x80U) != 0U) {
            reset_matrix(dev);
            return;
        }
        released &= state[col] == 0U;
    }
    if (dev->wait_for_release) {
        if (released) dev->wait_for_release = false;
        return;
    }
    process_matrix(dev, state);
}

static void worker(void *arg)
{
    tdeck_device_t *dev = arg;
    while (!dev->stop_requested) {
        poll_matrix(dev);
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TDECK_POLL_MS)) != 0U)
            reset_matrix(dev);
    }
    solar_os_input_composition_set_modifiers(dev->input_source, 0U);
    dev->worker_done = true;
    solar_os_task_delete_internal(NULL);
}

void solar_os_tdeck_keyboard_resume(void)
{
    if (device.active && device.worker_task != NULL)
        (void)xTaskNotifyGive(device.worker_task);
}

esp_err_t solar_os_tdeck_keyboard_attach(
    const char *name, const solar_os_expansion_binding_t *bindings,
    size_t binding_count)
{
    if (device.active || name == NULL || name[0] == '\0') return ESP_ERR_INVALID_STATE;
    char bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count, bus), TAG,
                        "invalid bindings");
    ESP_RETURN_ON_ERROR(solar_os_bus_i2c_probe(bus, SOLAR_OS_TDECK_KEYBOARD_ADDRESS),
                        TAG, "keyboard not found");
    memset(&device, 0, sizeof(device));
    device.active = true;
    strlcpy(device.name, name, sizeof(device.name));
    strlcpy(device.i2c_bus, bus, sizeof(device.i2c_bus));
    const uint8_t command = TDECK_MODE_RAW;
    esp_err_t err = solar_os_bus_i2c_transmit(bus, SOLAR_OS_TDECK_KEYBOARD_ADDRESS,
                                             &command, 1);
    if (err == ESP_OK) err = solar_os_input_keyboard_source_open(
        device.name, true, &device.input_source);
    if (err != ESP_OK) { memset(&device, 0, sizeof(device)); return err; }
    err = solar_os_backlight_register(set_backlight, &device);
    if (err == ESP_OK) {
        /* Keep Alt+B's native toggle level separate from the initial output.
         * Registering while boot-disabled sends zero without changing NVS. */
        solar_os_backlight_status_t light;
        err = solar_os_backlight_get_status(&light);
        if (err == ESP_OK) {
            const uint8_t preset[] = {0x02U,
                (uint8_t)((light.percent * 255U + 50U) / 100U)};
            err = solar_os_bus_i2c_transmit(bus, SOLAR_OS_TDECK_KEYBOARD_ADDRESS,
                                           preset, sizeof(preset));
        }
    }
    if (err != ESP_OK) {
        solar_os_backlight_unregister(&device);
        solar_os_input_source_close(device.input_source);
        memset(&device, 0, sizeof(device));
        return err;
    }
    if (solar_os_task_create_pinned_internal(worker, device.name, TDECK_TASK_STACK,
            &device, tskIDLE_PRIORITY + 1, &device.worker_task, tskNO_AFFINITY,
            SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        solar_os_backlight_unregister(&device);
        solar_os_input_source_close(device.input_source);
        memset(&device, 0, sizeof(device));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t solar_os_tdeck_keyboard_detach(const char *name)
{
    if (!device.active || name == NULL || strcmp(name, device.name) != 0)
        return ESP_ERR_NOT_FOUND;
    device.stop_requested = true;
    (void)xTaskNotifyGive(device.worker_task);
    if (!solar_os_task_wait_done(device.worker_task, &device.worker_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) return ESP_ERR_TIMEOUT;
    solar_os_backlight_unregister(&device);
    const uint8_t command = TDECK_MODE_KEY;
    (void)solar_os_bus_i2c_transmit(device.i2c_bus, SOLAR_OS_TDECK_KEYBOARD_ADDRESS,
                                   &command, 1);
    solar_os_input_source_close(device.input_source);
    memset(&device, 0, sizeof(device));
    return ESP_OK;
}

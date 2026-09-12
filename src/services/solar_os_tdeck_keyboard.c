#include "solar_os_tdeck_keyboard.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_board.h"
#include "solar_os_buses.h"
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

typedef struct { uint8_t normal; uint8_t symbol; } tdeck_key_t;
#ifndef SOLAR_OS_BOARD_TDECK_KEYMAP
/* Default raw-matrix map for LilyGO's T-Deck keyboard controller. Boards can
 * override this when their keycaps or matrix wiring differ. */
#define SOLAR_OS_BOARD_TDECK_KEYMAP \
    {{{0x71,0x23},{0x77,0x31},{0,0},{0x61,0x2a},{0,0},{0x20,0},{0,0x30}}, \
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
    solar_os_input_source_t input_source;
    TaskHandle_t worker_task;
} tdeck_device_t;

static const char *TAG = "tdeck_keyboard";
static tdeck_device_t device;

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

static void process_matrix(tdeck_device_t *dev,
                           const uint8_t state[TDECK_COLUMNS])
{
    const bool alt = (state[0] & (1U << 4U)) != 0U;
    const bool symbol = (state[0] & (1U << 2U)) != 0U;
    const bool shift = (state[1] & (1U << 6U)) != 0U ||
                       (state[2] & (1U << 3U)) != 0U;
    solar_os_input_composition_set_modifiers(
        dev->input_source,
        (alt ? SOLAR_OS_INPUT_MOD_LEFT_ALT : 0U) |
        (shift ? SOLAR_OS_INPUT_MOD_LEFT_SHIFT : 0U));
    for (size_t col = 0; col < TDECK_COLUMNS; col++) {
        const uint8_t pressed = state[col] & (uint8_t)~dev->previous[col];
        for (size_t row = 0; row < TDECK_ROWS; row++) {
            if ((pressed & (1U << row)) == 0U) continue;
            uint8_t key = symbol ? keymap[col][row].symbol : keymap[col][row].normal;
            if (key == 0U) continue;
            if (shift && key >= 'a' && key <= 'z') key = (uint8_t)(key - 'a' + 'A');
            key = solar_os_input_composition_apply(key);
            if (key != 0U) (void)solar_os_input_write_char(dev->input_source, (char)key);
        }
    }
    memcpy(dev->previous, state, sizeof(dev->previous));
}

static void worker(void *arg)
{
    tdeck_device_t *dev = arg;
    while (!dev->stop_requested) {
        uint8_t state[TDECK_COLUMNS] = {0};
        if (solar_os_bus_i2c_receive(dev->i2c_bus,
                                    SOLAR_OS_TDECK_KEYBOARD_ADDRESS,
                                    state, sizeof(state)) == ESP_OK) {
            process_matrix(dev, state);
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TDECK_POLL_MS));
    }
    solar_os_input_composition_set_modifiers(dev->input_source, 0U);
    dev->worker_done = true;
    solar_os_task_delete_internal(NULL);
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
    if (solar_os_task_create_pinned_internal(worker, device.name, TDECK_TASK_STACK,
            &device, tskIDLE_PRIORITY + 1, &device.worker_task, tskNO_AFFINITY,
            SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
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
    const uint8_t command = TDECK_MODE_KEY;
    (void)solar_os_bus_i2c_transmit(device.i2c_bus, SOLAR_OS_TDECK_KEYBOARD_ADDRESS,
                                   &command, 1);
    solar_os_input_source_close(device.input_source);
    memset(&device, 0, sizeof(device));
    return ESP_OK;
}

#include "solar_os_ps2_keyboard_device.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ps2_bus.h"
#include "solar_os_buses.h"
#include "solar_os_input.h"
#include "solar_os_ps2_keyboard.h"
#include "solar_os_task.h"

#define PS2_KEYBOARD_POLL_MS 2U
#define PS2_KEYBOARD_TASK_STACK 3072U
#define PS2_KEYBOARD_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define PS2_KEYBOARD_DEVICE_MAX 2U
#define PS2_KEYBOARD_USAGE_BITMAP_SIZE 32U

typedef struct {
    bool active;
    volatile bool stop_requested;
    volatile bool worker_done;
    bool caps_lock;
    uint8_t modifiers;
    uint8_t held[PS2_KEYBOARD_USAGE_BITMAP_SIZE];
    uint32_t transitions;
    uint32_t unsupported;
    uint32_t dropped;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char bus_name[SOLAR_OS_EXPANSION_TARGET_MAX];
    solar_os_input_source_t input_source;
    TaskHandle_t worker_task;
    solar_os_ps2_bus_t bus;
    solar_os_ps2_keyboard_decoder_t decoder;
} solar_os_ps2_keyboard_device_t;

static const char *TAG = "ps2-keyboard";
static solar_os_ps2_keyboard_device_t devices[PS2_KEYBOARD_DEVICE_MAX];

static solar_os_ps2_keyboard_device_t *find_device(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < PS2_KEYBOARD_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            return &devices[i];
        }
    }
    return NULL;
}

static solar_os_ps2_keyboard_device_t *alloc_device(void)
{
    for (size_t i = 0; i < PS2_KEYBOARD_DEVICE_MAX; i++) {
        if (!devices[i].active) {
            return &devices[i];
        }
    }
    return NULL;
}

static bool usage_held(const solar_os_ps2_keyboard_device_t *device,
                       uint16_t usage)
{
    return usage < PS2_KEYBOARD_USAGE_BITMAP_SIZE * 8U &&
        (device->held[usage / 8U] & (uint8_t)(1U << (usage % 8U))) != 0;
}

static void set_usage_held(solar_os_ps2_keyboard_device_t *device,
                           uint16_t usage,
                           bool held)
{
    if (usage >= PS2_KEYBOARD_USAGE_BITMAP_SIZE * 8U) {
        return;
    }
    const uint8_t mask = (uint8_t)(1U << (usage % 8U));
    if (held) {
        device->held[usage / 8U] |= mask;
    } else {
        device->held[usage / 8U] &= (uint8_t)~mask;
    }
}

static uint8_t modifier_bit(uint16_t usage)
{
    return usage >= 0xe0U && usage <= 0xe7U
        ? (uint8_t)(1U << (usage - 0xe0U))
        : 0;
}

static void emit_transition(solar_os_ps2_keyboard_device_t *device,
                            const solar_os_ps2_key_transition_t *transition)
{
    if (transition == NULL ||
        transition->usage >= PS2_KEYBOARD_USAGE_BITMAP_SIZE * 8U) {
        device->unsupported++;
        return;
    }
    const bool held = usage_held(device, transition->usage);
    if (held == transition->pressed) {
        return;
    }
    set_usage_held(device, transition->usage, transition->pressed);

    const uint8_t modifier = modifier_bit(transition->usage);
    if (modifier != 0) {
        if (transition->pressed) {
            device->modifiers |= modifier;
        } else {
            device->modifiers &= (uint8_t)~modifier;
        }
    }
    if (transition->usage == 0x39U && transition->pressed) {
        device->caps_lock = !device->caps_lock;
    }

    const uint8_t key = solar_os_input_translate_hid_usage(
        transition->usage,
        device->modifiers,
        device->caps_lock);
    if (solar_os_input_write_key(
            device->input_source,
            transition->usage,
            transition->usage,
            key,
            device->modifiers,
            transition->pressed ? SOLAR_OS_INPUT_KEY_PRESS :
                                  SOLAR_OS_INPUT_KEY_RELEASE) == ESP_OK) {
        device->transitions++;
    } else {
        device->dropped++;
    }
}

static void ps2_keyboard_worker(void *arg)
{
    solar_os_ps2_keyboard_device_t *device = arg;
    while (!device->stop_requested) {
        uint8_t bytes[SOLAR_OS_PS2_RX_BUFFER_SIZE];
        const size_t count = solar_os_ps2_bus_read(&device->bus,
                                                   bytes,
                                                   sizeof(bytes));
        for (size_t i = 0; i < count; i++) {
            solar_os_ps2_key_transition_t transition;
            const solar_os_ps2_decode_result_t result =
                solar_os_ps2_keyboard_decode(&device->decoder,
                                              bytes[i],
                                              &transition);
            if (result == SOLAR_OS_PS2_DECODE_KEY) {
                emit_transition(device, &transition);
            } else if (result == SOLAR_OS_PS2_DECODE_UNSUPPORTED) {
                device->unsupported++;
            }
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PS2_KEYBOARD_POLL_MS));
    }

    device->worker_done = true;
    solar_os_task_delete_internal(NULL);
}

static void clear_device(solar_os_ps2_keyboard_device_t *device)
{
    if (device == NULL) {
        return;
    }
    solar_os_ps2_bus_stop(&device->bus);
    if (device->input_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        solar_os_input_source_close(device->input_source);
    }
    memset(device, 0, sizeof(*device));
}

static esp_err_t parse_bus(const solar_os_expansion_binding_t *bindings,
                           size_t binding_count,
                           char *bus_name,
                           size_t bus_name_len)
{
    if (bindings == NULL || binding_count != 1 || bus_name == NULL ||
        bindings[0].kind != SOLAR_OS_EXPANSION_BINDING_PS2_BUS ||
        bindings[0].target[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(bus_name, bindings[0].target, bus_name_len);
    return ESP_OK;
}

esp_err_t solar_os_ps2_keyboard_attach(
    const char *name,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (find_device(name) != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    solar_os_ps2_keyboard_device_t *device = alloc_device();
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char bus_name[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    ESP_RETURN_ON_ERROR(parse_bus(bindings,
                                  binding_count,
                                  bus_name,
                                  sizeof(bus_name)),
                        TAG,
                        "invalid bindings");
    solar_os_bus_info_t info;
    if (!solar_os_bus_find(bus_name, SOLAR_OS_BUS_PROTOCOL_PS2, &info)) {
        return ESP_ERR_NOT_FOUND;
    }

    memset(device, 0, sizeof(*device));
    device->active = true;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->bus_name, bus_name, sizeof(device->bus_name));
    esp_err_t err = solar_os_input_keyboard_source_open(device->name,
                                                        true,
                                                        &device->input_source);
    if (err == ESP_OK) {
        err = solar_os_ps2_bus_start(&device->bus, &info.config.ps2);
    }
    if (err != ESP_OK) {
        clear_device(device);
        return err;
    }
    solar_os_ps2_keyboard_decoder_reset(&device->decoder);
    if (solar_os_task_create_pinned_internal(ps2_keyboard_worker,
                                             device->name,
                                             PS2_KEYBOARD_TASK_STACK,
                                             device,
                                             PS2_KEYBOARD_TASK_PRIORITY,
                                             &device->worker_task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        clear_device(device);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "%s attached on %s: clock=GPIO%d data=GPIO%d",
             name,
             bus_name,
             info.config.ps2.clock_pin,
             info.config.ps2.data_pin);
    return ESP_OK;
}

esp_err_t solar_os_ps2_keyboard_detach(const char *name)
{
    solar_os_ps2_keyboard_device_t *device = find_device(name);
    if (device == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    device->stop_requested = true;
    if (device->worker_task != NULL) {
        (void)xTaskNotifyGive(device->worker_task);
    }
    if (!solar_os_task_wait_done(device->worker_task,
                                 &device->worker_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }

    solar_os_ps2_bus_stats_t stats;
    solar_os_ps2_bus_get_stats(&device->bus, &stats);
    ESP_LOGI(TAG,
             "%s detached: bytes=%u transitions=%u unsupported=%u dropped=%u frame_errors=%u overruns=%u",
             name,
             (unsigned)stats.bytes,
             (unsigned)device->transitions,
             (unsigned)device->unsupported,
             (unsigned)device->dropped,
             (unsigned)stats.frame_errors,
             (unsigned)stats.overruns);
    clear_device(device);
    return ESP_OK;
}

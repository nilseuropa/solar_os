#include "solar_os_ps2_mouse_device.h"

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
#include "solar_os_ps2_mouse.h"
#include "solar_os_task.h"

#define PS2_MOUSE_POLL_MS 2U
#define PS2_MOUSE_COMMAND_TIMEOUT_MS 200U
#define PS2_MOUSE_STARTUP_SETTLE_MS 20U
#define PS2_MOUSE_TASK_STACK 3072U
#define PS2_MOUSE_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define PS2_MOUSE_DEVICE_MAX 2U
#define PS2_MOUSE_SET_DEFAULTS 0xf6U
#define PS2_MOUSE_DISABLE_REPORTING 0xf5U
#define PS2_MOUSE_ENABLE_REPORTING 0xf4U

typedef struct {
    bool active;
    volatile bool stop_requested;
    volatile bool worker_done;
    uint8_t buttons;
    uint32_t packets;
    uint32_t unsupported;
    uint32_t dropped;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char bus_name[SOLAR_OS_EXPANSION_TARGET_MAX];
    solar_os_input_source_t input_source;
    TaskHandle_t worker_task;
    solar_os_ps2_bus_t bus;
    solar_os_ps2_mouse_decoder_t decoder;
} solar_os_ps2_mouse_device_t;

static const char *TAG = "ps2-mouse";
static solar_os_ps2_mouse_device_t devices[PS2_MOUSE_DEVICE_MAX];

static solar_os_ps2_mouse_device_t *find_device(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < PS2_MOUSE_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            return &devices[i];
        }
    }
    return NULL;
}

static solar_os_ps2_mouse_device_t *alloc_device(void)
{
    for (size_t i = 0; i < PS2_MOUSE_DEVICE_MAX; i++) {
        if (!devices[i].active) {
            return &devices[i];
        }
    }
    return NULL;
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

static void emit_packet(solar_os_ps2_mouse_device_t *device,
                        const solar_os_ps2_mouse_packet_t *packet)
{
    if (packet->delta_x == 0 && packet->delta_y == 0 &&
        packet->buttons == device->buttons) {
        return;
    }
    solar_os_input_pointer_action_t action = SOLAR_OS_INPUT_POINTER_MOVE;
    if (packet->buttons != device->buttons) {
        action = (packet->buttons & (uint8_t)~device->buttons) != 0
            ? SOLAR_OS_INPUT_POINTER_PRESS
            : SOLAR_OS_INPUT_POINTER_RELEASE;
    }
    const solar_os_input_pointer_event_t event = {
        .pointer_id = 0,
        .buttons = packet->buttons,
        .mode = SOLAR_OS_INPUT_POINTER_RELATIVE,
        .action = action,
        .delta_x = packet->delta_x,
        .delta_y = packet->delta_y,
    };
    if (solar_os_input_write_pointer(device->input_source, &event) == ESP_OK) {
        device->buttons = packet->buttons;
        device->packets++;
    } else {
        device->dropped++;
    }
}

static void ps2_mouse_worker(void *arg)
{
    solar_os_ps2_mouse_device_t *device = arg;
    while (!device->stop_requested) {
        uint8_t bytes[SOLAR_OS_PS2_RX_BUFFER_SIZE];
        const size_t count = solar_os_ps2_bus_read(&device->bus,
                                                   bytes,
                                                   sizeof(bytes));
        for (size_t i = 0; i < count; i++) {
            solar_os_ps2_mouse_packet_t packet;
            const solar_os_ps2_mouse_decode_result_t result =
                solar_os_ps2_mouse_decode(&device->decoder,
                                          bytes[i],
                                          &packet);
            if (result == SOLAR_OS_PS2_MOUSE_DECODE_PACKET) {
                emit_packet(device, &packet);
            } else if (result == SOLAR_OS_PS2_MOUSE_DECODE_UNSUPPORTED) {
                device->unsupported++;
            }
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PS2_MOUSE_POLL_MS));
    }

    device->worker_done = true;
    solar_os_task_delete_internal(NULL);
}

static void clear_device(solar_os_ps2_mouse_device_t *device)
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

static void flush_bus(solar_os_ps2_bus_t *bus)
{
    uint8_t bytes[SOLAR_OS_PS2_RX_BUFFER_SIZE];
    while (solar_os_ps2_bus_read(bus, bytes, sizeof(bytes)) > 0) {
    }
}

esp_err_t solar_os_ps2_mouse_attach(
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
    solar_os_ps2_mouse_device_t *device = alloc_device();
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
    esp_err_t err = solar_os_ps2_bus_start(&device->bus, &info.config.ps2);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(PS2_MOUSE_STARTUP_SETTLE_MS));
        flush_bus(&device->bus);
        err = solar_os_ps2_bus_command(&device->bus,
                                       PS2_MOUSE_SET_DEFAULTS,
                                       PS2_MOUSE_COMMAND_TIMEOUT_MS);
    }
    if (err == ESP_OK) {
        err = solar_os_ps2_bus_command(&device->bus,
                                       PS2_MOUSE_ENABLE_REPORTING,
                                       PS2_MOUSE_COMMAND_TIMEOUT_MS);
    }
    if (err == ESP_OK) {
        err = solar_os_input_mouse_source_open(device->name,
                                               &device->input_source);
    }
    if (err != ESP_OK) {
        clear_device(device);
        return err;
    }
    solar_os_ps2_mouse_decoder_reset(&device->decoder);
    if (solar_os_task_create_pinned_internal(ps2_mouse_worker,
                                             device->name,
                                             PS2_MOUSE_TASK_STACK,
                                             device,
                                             PS2_MOUSE_TASK_PRIORITY,
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

esp_err_t solar_os_ps2_mouse_detach(const char *name)
{
    solar_os_ps2_mouse_device_t *device = find_device(name);
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
    flush_bus(&device->bus);
    const esp_err_t disable_err = solar_os_ps2_bus_command(
        &device->bus,
        PS2_MOUSE_DISABLE_REPORTING,
        PS2_MOUSE_COMMAND_TIMEOUT_MS);
    if (disable_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "%s could not disable reporting: %s",
                 name,
                 esp_err_to_name(disable_err));
    }

    solar_os_ps2_bus_stats_t stats;
    solar_os_ps2_bus_get_stats(&device->bus, &stats);
    ESP_LOGI(TAG,
             "%s detached: bytes=%u packets=%u unsupported=%u dropped=%u frame_errors=%u overruns=%u command_errors=%u",
             name,
             (unsigned)stats.bytes,
             (unsigned)device->packets,
             (unsigned)device->unsupported,
             (unsigned)device->dropped,
             (unsigned)stats.frame_errors,
             (unsigned)stats.overruns,
             (unsigned)stats.command_errors);
    clear_device(device);
    return ESP_OK;
}

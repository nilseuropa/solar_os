#include "solar_os_cardkb.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_buses.h"
#include "solar_os_cardkb_codec.h"
#include "solar_os_input.h"
#include "solar_os_task.h"

#define CARDKB_POLL_MS 10U
#define CARDKB_TASK_STACK 3072U
#define CARDKB_TASK_PRIORITY (tskIDLE_PRIORITY + 1)
#define CARDKB_DEVICE_MAX 4U

typedef struct {
    bool active;
    volatile bool stop_requested;
    volatile bool worker_done;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    solar_os_input_source_t input_source;
    TaskHandle_t worker_task;
    uint32_t keys;
    uint32_t unsupported;
    uint32_t dropped;
    uint32_t bus_errors;
} solar_os_cardkb_device_t;

static const char *TAG = "cardkb";
static EXT_RAM_BSS_ATTR solar_os_cardkb_device_t cardkb_devices[CARDKB_DEVICE_MAX];

static solar_os_cardkb_device_t *find_device(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < CARDKB_DEVICE_MAX; i++) {
        if (cardkb_devices[i].active &&
            strcmp(cardkb_devices[i].name, name) == 0) {
            return &cardkb_devices[i];
        }
    }
    return NULL;
}

static solar_os_cardkb_device_t *alloc_device(void)
{
    for (size_t i = 0; i < CARDKB_DEVICE_MAX; i++) {
        if (!cardkb_devices[i].active) {
            return &cardkb_devices[i];
        }
    }
    return NULL;
}

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *i2c_bus,
                                size_t i2c_bus_len,
                                uint8_t *address)
{
    bool have_i2c = false;
    bool have_address = false;

    if (bindings == NULL || i2c_bus == NULL || address == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_bus[0] = '\0';
    *address = 0;

    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        switch (binding->kind) {
        case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
            if (have_i2c) {
                return ESP_ERR_INVALID_ARG;
            }
            strlcpy(i2c_bus, binding->target, i2c_bus_len);
            have_i2c = true;
            break;
        case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
            if (have_address || binding->value != SOLAR_OS_CARDKB_ADDRESS) {
                return ESP_ERR_INVALID_ARG;
            }
            *address = (uint8_t)binding->value;
            have_address = true;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
        }
    }

    return have_i2c && have_address &&
            solar_os_expansion_find_i2c_bus(i2c_bus, NULL, NULL)
        ? ESP_OK
        : ESP_ERR_INVALID_ARG;
}

static void cardkb_worker(void *arg)
{
    solar_os_cardkb_device_t *device = arg;
    bool bus_error_reported = false;

    while (!device->stop_requested) {
        uint8_t value = 0;
        const esp_err_t read_err = solar_os_bus_i2c_receive(device->i2c_bus,
                                                            device->address,
                                                            &value,
                                                            sizeof(value));
        if (read_err == ESP_OK) {
            bus_error_reported = false;
            if (value != 0) {
                uint8_t key = 0;
                if (!solar_os_cardkb_decode(value, &key)) {
                    device->unsupported++;
                } else if (solar_os_input_write_char(device->input_source,
                                                      (char)key) != ESP_OK) {
                    device->dropped++;
                } else {
                    device->keys++;
                }
            }
        } else {
            device->bus_errors++;
            if (!bus_error_reported) {
                ESP_LOGW(TAG,
                         "%s read failed on %s: %s",
                         device->name,
                         device->i2c_bus,
                         esp_err_to_name(read_err));
                bus_error_reported = true;
            }
        }

        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CARDKB_POLL_MS));
    }

    device->worker_done = true;
    solar_os_task_delete_internal(NULL);
}

static void clear_device(solar_os_cardkb_device_t *device)
{
    if (device == NULL) {
        return;
    }
    if (device->input_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        solar_os_input_source_close(device->input_source);
    }
    memset(device, 0, sizeof(*device));
}

esp_err_t solar_os_cardkb_attach(const char *name,
                                 const solar_os_expansion_binding_t *bindings,
                                 size_t binding_count)
{
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    uint8_t address = 0;

    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (find_device(name) != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    solar_os_cardkb_device_t *device = alloc_device();
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(parse_bindings(bindings,
                                       binding_count,
                                       i2c_bus,
                                       sizeof(i2c_bus),
                                       &address),
                        TAG,
                        "invalid bindings");
    ESP_RETURN_ON_ERROR(solar_os_bus_i2c_probe(i2c_bus, address),
                        TAG,
                        "CardKB not found");

    memset(device, 0, sizeof(*device));
    device->active = true;
    device->address = address;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->i2c_bus, i2c_bus, sizeof(device->i2c_bus));

    esp_err_t err = solar_os_input_keyboard_source_open(device->name,
                                                       true,
                                                       &device->input_source);
    if (err != ESP_OK) {
        clear_device(device);
        return err;
    }
    if (solar_os_task_create_pinned_internal(cardkb_worker,
                                             device->name,
                                             CARDKB_TASK_STACK,
                                             device,
                                             CARDKB_TASK_PRIORITY,
                                             &device->worker_task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        clear_device(device);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "%s attached on %s address 0x%02x",
             name,
             i2c_bus,
             address);
    return ESP_OK;
}

esp_err_t solar_os_cardkb_detach(const char *name)
{
    solar_os_cardkb_device_t *device = find_device(name);
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

    ESP_LOGI(TAG,
             "%s detached: %lu keys, %lu unsupported, %lu dropped, %lu bus errors",
             name,
             (unsigned long)device->keys,
             (unsigned long)device->unsupported,
             (unsigned long)device->dropped,
             (unsigned long)device->bus_errors);
    clear_device(device);
    return ESP_OK;
}

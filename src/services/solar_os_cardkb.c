#include "solar_os_cardkb.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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
static solar_os_cardkb_device_t cardkb;

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

static void clear_device(void)
{
    if (cardkb.input_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        solar_os_input_source_close(cardkb.input_source);
    }
    memset(&cardkb, 0, sizeof(cardkb));
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
    if (cardkb.active) {
        return ESP_ERR_INVALID_STATE;
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

    cardkb.active = true;
    cardkb.address = address;
    strlcpy(cardkb.name, name, sizeof(cardkb.name));
    strlcpy(cardkb.i2c_bus, i2c_bus, sizeof(cardkb.i2c_bus));

    esp_err_t err = solar_os_input_source_open("cardkb", &cardkb.input_source);
    if (err != ESP_OK) {
        clear_device();
        return err;
    }
    if (solar_os_task_create_pinned_internal(cardkb_worker,
                                             "cardkb",
                                             CARDKB_TASK_STACK,
                                             &cardkb,
                                             CARDKB_TASK_PRIORITY,
                                             &cardkb.worker_task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        clear_device();
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
    if (!cardkb.active || name == NULL || strcmp(cardkb.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    cardkb.stop_requested = true;
    if (cardkb.worker_task != NULL) {
        (void)xTaskNotifyGive(cardkb.worker_task);
    }
    if (!solar_os_task_wait_done(cardkb.worker_task,
                                 &cardkb.worker_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG,
             "%s detached: %lu keys, %lu unsupported, %lu dropped, %lu bus errors",
             name,
             (unsigned long)cardkb.keys,
             (unsigned long)cardkb.unsupported,
             (unsigned long)cardkb.dropped,
             (unsigned long)cardkb.bus_errors);
    clear_device();
    return ESP_OK;
}

#include "solar_os_gpio_keys.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_gpio.h"
#include "solar_os_input.h"
#include "solar_os_keys.h"
#include "solar_os_task.h"

#define GPIO_KEYS_MAX SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX
#define GPIO_KEYS_DEVICE_MAX 4U
#define GPIO_KEYS_DEBOUNCE_MS 25U
#define GPIO_KEYS_POLL_MS 10U
#define GPIO_KEYS_TASK_STACK 3072U
#define GPIO_KEYS_TASK_PRIORITY (tskIDLE_PRIORITY + 1)

typedef struct {
    int pin;
    uint8_t key;
    bool last_raw_pressed;
    bool stable_pressed;
    uint32_t raw_changed_ms;
} gpio_key_mapping_t;

typedef struct {
    bool active;
    volatile bool stop_requested;
    volatile bool worker_done;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    size_t mapping_count;
    gpio_key_mapping_t mappings[GPIO_KEYS_MAX];
    solar_os_input_source_t input_source;
    TaskHandle_t worker_task;
    uint32_t presses;
    uint32_t dropped;
    uint32_t read_errors;
} solar_os_gpio_keys_device_t;

static const char *TAG = "gpio-keys";
static EXT_RAM_BSS_ATTR solar_os_gpio_keys_device_t devices[GPIO_KEYS_DEVICE_MAX];

static solar_os_gpio_keys_device_t *find_device(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < GPIO_KEYS_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            return &devices[i];
        }
    }
    return NULL;
}

static solar_os_gpio_keys_device_t *alloc_device(void)
{
    for (size_t i = 0; i < GPIO_KEYS_DEVICE_MAX; i++) {
        if (!devices[i].active) {
            return &devices[i];
        }
    }
    return NULL;
}

static esp_err_t parse_bindings(
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count,
    solar_os_gpio_keys_device_t *device)
{
    if (bindings == NULL || binding_count == 0 || binding_count > GPIO_KEYS_MAX ||
        device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        uint8_t key = 0;
        if (binding->kind != SOLAR_OS_EXPANSION_BINDING_GPIO ||
            binding->role[0] == '\0' ||
            !solar_os_key_parse(binding->role, &key)) {
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t j = 0; j < i; j++) {
            if (device->mappings[j].pin == binding->value) {
                return ESP_ERR_INVALID_ARG;
            }
        }
        device->mappings[i] = (gpio_key_mapping_t) {
            .pin = binding->value,
            .key = key,
        };
    }
    device->mapping_count = binding_count;
    return ESP_OK;
}

static void reset_pins(solar_os_gpio_keys_device_t *device)
{
    for (size_t i = 0; i < device->mapping_count; i++) {
        (void)solar_os_gpio_release_owned(device->mappings[i].pin, device->name);
    }
}

static void clear_device(solar_os_gpio_keys_device_t *device)
{
    if (device == NULL) {
        return;
    }
    if (device->input_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        solar_os_input_source_close(device->input_source);
    }
    reset_pins(device);
    memset(device, 0, sizeof(*device));
}

static void gpio_keys_worker(void *arg)
{
    solar_os_gpio_keys_device_t *device = arg;
    TickType_t last_wake = xTaskGetTickCount();
    while (!device->stop_requested) {
        const uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        for (size_t i = 0; i < device->mapping_count; i++) {
            gpio_key_mapping_t *mapping = &device->mappings[i];
            bool level = true;
            if (solar_os_gpio_read_owned(mapping->pin,
                                         device->name,
                                         &level) != ESP_OK) {
                device->read_errors++;
                continue;
            }
            const bool pressed = !level;
            if (pressed != mapping->last_raw_pressed) {
                mapping->last_raw_pressed = pressed;
                mapping->raw_changed_ms = now_ms;
                continue;
            }
            if (pressed == mapping->stable_pressed ||
                (uint32_t)(now_ms - mapping->raw_changed_ms) <
                    GPIO_KEYS_DEBOUNCE_MS) {
                continue;
            }

            mapping->stable_pressed = pressed;
            if (solar_os_input_write_key(
                    device->input_source,
                    (uint16_t)mapping->pin + 1U,
                    SOLAR_OS_INPUT_USAGE_NONE,
                    mapping->key,
                    0,
                    pressed ? SOLAR_OS_INPUT_KEY_PRESS :
                              SOLAR_OS_INPUT_KEY_RELEASE) == ESP_OK) {
                if (pressed) {
                    device->presses++;
                }
            } else {
                device->dropped++;
            }
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(GPIO_KEYS_POLL_MS));
    }

    device->worker_done = true;
    solar_os_task_delete_internal(NULL);
}

esp_err_t solar_os_gpio_keys_attach(const char *name,
                                    const solar_os_expansion_binding_t *bindings,
                                    size_t binding_count)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (find_device(name) != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    solar_os_gpio_keys_device_t *device = alloc_device();
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(device, 0, sizeof(*device));
    device->active = true;
    strlcpy(device->name, name, sizeof(device->name));
    esp_err_t err = parse_bindings(bindings, binding_count, device);
    if (err != ESP_OK) {
        memset(device, 0, sizeof(*device));
        return err;
    }

    for (size_t i = 0; i < device->mapping_count; i++) {
        gpio_key_mapping_t *mapping = &device->mappings[i];
        err = solar_os_gpio_configure_owned(mapping->pin,
                                            SOLAR_OS_GPIO_MODE_INPUT,
                                            SOLAR_OS_GPIO_PULL_UP,
                                            device->name);
        bool level = true;
        if (err == ESP_OK) {
            err = solar_os_gpio_read_owned(mapping->pin, device->name, &level);
        }
        if (err != ESP_OK) {
            clear_device(device);
            return err;
        }
        mapping->last_raw_pressed = !level;
        mapping->stable_pressed = !level;
    }

    err = solar_os_input_key_source_open(device->name,
                                         SOLAR_OS_INPUT_SOURCE_KEYBOARD,
                                         &device->input_source);
    if (err != ESP_OK) {
        clear_device(device);
        return err;
    }
    if (solar_os_task_create_pinned_internal(gpio_keys_worker,
                                             device->name,
                                             GPIO_KEYS_TASK_STACK,
                                             device,
                                             GPIO_KEYS_TASK_PRIORITY,
                                             &device->worker_task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        clear_device(device);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "%s attached with %u pull-up keys",
             name,
             (unsigned)device->mapping_count);
    return ESP_OK;
}

esp_err_t solar_os_gpio_keys_detach(const char *name)
{
    solar_os_gpio_keys_device_t *device = find_device(name);
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
             "%s detached: presses=%u dropped=%u read_errors=%u",
             name,
             (unsigned)device->presses,
             (unsigned)device->dropped,
             (unsigned)device->read_errors);
    clear_device(device);
    return ESP_OK;
}

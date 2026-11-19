#include "solar_os_uart_keyboard.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_buses.h"
#include "solar_os_input.h"
#include "solar_os_key_codes.h"
#include "solar_os_task.h"
#include "solar_os_vt100.h"

/* Keys typed in a terminal on a UART (VT100 byte stream) feed the shared
 * input service, like a CardKB. Under QEMU this is the way to type into the
 * display shell: QEMU's ESP32-S3 has no I2C, USB or PS/2 input. */

#define UART_KEYBOARD_READ_TIMEOUT_MS 20U
#define UART_KEYBOARD_ESC_FLUSH_MS 40U
#define UART_KEYBOARD_TASK_STACK 3072U
#define UART_KEYBOARD_TASK_PRIORITY (tskIDLE_PRIORITY + 1)
#define UART_KEYBOARD_DEVICE_MAX 2U

typedef struct {
    bool active;
    volatile bool stop_requested;
    volatile bool worker_done;
    bool last_was_cr;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char uart_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    solar_os_input_source_t input_source;
    solar_os_vt100_input_t vt100;
    TaskHandle_t worker_task;
    uint32_t keys;
    uint32_t dropped;
    uint32_t bus_errors;
} uart_keyboard_device_t;

static const char *TAG = "uart_keyboard";
static EXT_RAM_BSS_ATTR uart_keyboard_device_t devices[UART_KEYBOARD_DEVICE_MAX];

static uart_keyboard_device_t *find_device(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < UART_KEYBOARD_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            return &devices[i];
        }
    }
    return NULL;
}

static uart_keyboard_device_t *alloc_device(void)
{
    for (size_t i = 0; i < UART_KEYBOARD_DEVICE_MAX; i++) {
        if (!devices[i].active) {
            return &devices[i];
        }
    }
    return NULL;
}

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *uart_bus,
                                size_t uart_bus_len)
{
    bool have_uart = false;

    if (bindings == NULL || uart_bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uart_bus[0] = '\0';
    for (size_t i = 0; i < binding_count; i++) {
        if (bindings[i].kind != SOLAR_OS_EXPANSION_BINDING_UART_PORT || have_uart) {
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(uart_bus, bindings[i].target, uart_bus_len);
        have_uart = true;
    }
    return have_uart && solar_os_expansion_find_uart_port(uart_bus, NULL, NULL)
        ? ESP_OK
        : ESP_ERR_INVALID_ARG;
}

/* Terminals send Enter as CR or CR LF; SolarOS Enter is LF. */
static bool emit_key(char ch, void *user)
{
    uart_keyboard_device_t *device = user;
    if (ch == '\n' && device->last_was_cr) {
        device->last_was_cr = false;
        return true;
    }
    device->last_was_cr = ch == '\r';
    const char key = ch == '\r' ? (char)SOLAR_OS_KEY_ENTER : ch;
    if (solar_os_input_write_char(device->input_source, key) != ESP_OK) {
        device->dropped++;
    } else {
        device->keys++;
    }
    return true;
}

static void uart_keyboard_worker(void *arg)
{
    uart_keyboard_device_t *device = arg;
    bool bus_error_reported = false;
    TickType_t last_input = xTaskGetTickCount();

    solar_os_vt100_input_init(&device->vt100);
    while (!device->stop_requested) {
        uint8_t buffer[32];
        size_t read_len = 0;
        const esp_err_t err = solar_os_bus_uart_read(device->uart_bus,
                                                     buffer,
                                                     sizeof(buffer),
                                                     UART_KEYBOARD_READ_TIMEOUT_MS,
                                                     &read_len);
        const TickType_t now = xTaskGetTickCount();
        if (err == ESP_OK && read_len > 0) {
            bus_error_reported = false;
            (void)solar_os_vt100_input_feed(&device->vt100, buffer, read_len,
                                            emit_key, device);
            last_input = now;
        } else if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            device->bus_errors++;
            if (!bus_error_reported) {
                ESP_LOGW(TAG, "%s read failed on %s: %s",
                         device->name, device->uart_bus, esp_err_to_name(err));
                bus_error_reported = true;
            }
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(UART_KEYBOARD_READ_TIMEOUT_MS));
        }
        /* A lone Esc has no follow-up byte; send it after a short pause. */
        if (solar_os_vt100_input_pending(&device->vt100) &&
            (now - last_input) >= pdMS_TO_TICKS(UART_KEYBOARD_ESC_FLUSH_MS)) {
            (void)solar_os_vt100_input_flush(&device->vt100, emit_key, device);
        }
    }

    device->worker_done = true;
    solar_os_task_delete_internal(NULL);
}

static void clear_device(uart_keyboard_device_t *device)
{
    if (device->input_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        solar_os_input_source_close(device->input_source);
    }
    memset(device, 0, sizeof(*device));
}

esp_err_t solar_os_uart_keyboard_attach(const char *name,
                                        const solar_os_expansion_binding_t *bindings,
                                        size_t binding_count)
{
    char uart_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};

    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (find_device(name) != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uart_keyboard_device_t *device = alloc_device();
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count,
                                       uart_bus, sizeof(uart_bus)),
                        TAG, "invalid bindings");

    memset(device, 0, sizeof(*device));
    device->active = true;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->uart_bus, uart_bus, sizeof(device->uart_bus));

    esp_err_t err = solar_os_input_keyboard_source_open(device->name,
                                                       true,
                                                       &device->input_source);
    if (err != ESP_OK) {
        clear_device(device);
        return err;
    }
    if (solar_os_task_create_pinned_internal(uart_keyboard_worker,
                                             device->name,
                                             UART_KEYBOARD_TASK_STACK,
                                             device,
                                             UART_KEYBOARD_TASK_PRIORITY,
                                             &device->worker_task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        clear_device(device);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "%s attached on %s", name, uart_bus);
    return ESP_OK;
}

esp_err_t solar_os_uart_keyboard_detach(const char *name)
{
    uart_keyboard_device_t *device = find_device(name);
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

    ESP_LOGI(TAG, "%s detached: %lu keys, %lu dropped, %lu bus errors",
             name,
             (unsigned long)device->keys,
             (unsigned long)device->dropped,
             (unsigned long)device->bus_errors);
    clear_device(device);
    return ESP_OK;
}

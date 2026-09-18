#include "solar_os_sim7670.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_buses.h"
#include "solar_os_gnss.h"

#define SIM7670_DEVICE_MAX 2U

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char uart_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    bool gnss_powered;
    sim7670_t modem;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
} solar_os_sim7670_device_t;

static const char *TAG = "sim7670";
static solar_os_sim7670_device_t devices[SIM7670_DEVICE_MAX];

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
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_UART_PORT && !have_uart) {
            strlcpy(uart_bus, binding->target, uart_bus_len);
            have_uart = true;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return have_uart && solar_os_expansion_find_uart_port(uart_bus, NULL, NULL)
        ? ESP_OK
        : ESP_ERR_INVALID_ARG;
}

static esp_err_t modem_write(void *user,
                             const uint8_t *data,
                             size_t len,
                             size_t *written)
{
    solar_os_sim7670_device_t *device = user;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    return solar_os_bus_uart_write(device->uart_bus, data, len, written);
}

static esp_err_t modem_read(void *user,
                            uint8_t *data,
                            size_t len,
                            uint32_t timeout_ms,
                            size_t *read_len)
{
    solar_os_sim7670_device_t *device = user;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    return solar_os_bus_uart_read(device->uart_bus,
                                  data,
                                  len,
                                  timeout_ms,
                                  read_len);
}

static solar_os_sim7670_device_t *find_device(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < SIM7670_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            return &devices[i];
        }
    }
    return NULL;
}

static esp_err_t gnss_read_fix(void *ctx,
                               uint32_t timeout_ms,
                               solar_os_gnss_fix_t *fix)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active || fix == NULL || timeout_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    if (!device->gnss_powered) {
        xSemaphoreGive(device->mutex);
        return ESP_ERR_INVALID_STATE;
    }
    sim7670_gnss_fix_t modem_fix;
    const esp_err_t ret = sim7670_read_gnss_fix(&device->modem,
                                                timeout_ms,
                                                &modem_fix);
    if (ret == ESP_OK) {
        *fix = (solar_os_gnss_fix_t) {
            .valid = modem_fix.valid,
            .time_valid = modem_fix.time_valid,
            .year = modem_fix.year,
            .month = modem_fix.month,
            .day = modem_fix.day,
            .hour = modem_fix.hour,
            .minute = modem_fix.minute,
            .second = modem_fix.second,
            .fix_type = modem_fix.valid ? 3U : 0U,
            .latitude_deg_e7 = modem_fix.latitude_deg_e7,
            .longitude_deg_e7 = modem_fix.longitude_deg_e7,
            .height_msl_mm = modem_fix.height_msl_mm,
        };
    }
    xSemaphoreGive(device->mutex);
    return ret;
}

static esp_err_t gnss_set_power(void *ctx, bool enabled)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = sim7670_set_gnss_power(&device->modem, enabled);
    if (ret == ESP_OK) {
        device->gnss_powered = enabled;
    }
    xSemaphoreGive(device->mutex);
    return ret;
}

static const solar_os_gnss_ops_t gnss_ops = {
    .read_fix = gnss_read_fix,
    .set_power = gnss_set_power,
};

esp_err_t solar_os_sim7670_attach(
    const char *name,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_sim7670_device_t *device = NULL;
    for (size_t i = 0; i < SIM7670_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!devices[i].active && device == NULL) {
            device = &devices[i];
        }
    }
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char uart_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    ESP_RETURN_ON_ERROR(parse_bindings(bindings,
                                       binding_count,
                                       uart_bus,
                                       sizeof(uart_bus)),
                        TAG,
                        "invalid bindings");

    memset(device, 0, sizeof(*device));
    device->active = true;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->uart_bus, uart_bus, sizeof(device->uart_bus));
    device->mutex = xSemaphoreCreateMutexStatic(&device->mutex_storage);
    if (device->mutex == NULL) {
        memset(device, 0, sizeof(*device));
        return ESP_ERR_NO_MEM;
    }
    const sim7670_io_t io = {
        .write = modem_write,
        .read = modem_read,
        .user = device,
    };
    esp_err_t ret = sim7670_init(&device->modem, &io);
    if (ret != ESP_OK) {
        memset(device, 0, sizeof(*device));
        return ret;
    }

    const solar_os_gnss_registration_t registration = {
        .name = name,
        .driver = "sim7670",
        .ops = &gnss_ops,
        .ctx = device,
        .powered = false,
    };
    ret = solar_os_gnss_register(&registration);
    if (ret != ESP_OK) {
        memset(device, 0, sizeof(*device));
        return ret;
    }
    ESP_LOGI(TAG, "%s attached on %s", name, uart_bus);
    return ESP_OK;
}

esp_err_t solar_os_sim7670_detach(const char *name)
{
    solar_os_sim7670_device_t *device = find_device(name);
    if (device == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(solar_os_gnss_unregister(name),
                        TAG,
                        "GNSS unregister failed");
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    device->active = false;
    xSemaphoreGive(device->mutex);
    memset(device, 0, sizeof(*device));
    return ESP_OK;
}

size_t solar_os_sim7670_count(void)
{
    size_t count = 0U;
    for (size_t i = 0; i < SIM7670_DEVICE_MAX; i++) {
        count += devices[i].active ? 1U : 0U;
    }
    return count;
}

bool solar_os_sim7670_get(size_t index, solar_os_sim7670_info_t *info)
{
    if (info == NULL) {
        return false;
    }
    size_t current = 0U;
    for (size_t i = 0; i < SIM7670_DEVICE_MAX; i++) {
        if (!devices[i].active) {
            continue;
        }
        if (current++ == index) {
            xSemaphoreTake(devices[i].mutex, portMAX_DELAY);
            *info = (solar_os_sim7670_info_t) {
                .gnss_powered = devices[i].gnss_powered,
            };
            strlcpy(info->name, devices[i].name, sizeof(info->name));
            strlcpy(info->uart_bus,
                    devices[i].uart_bus,
                    sizeof(info->uart_bus));
            xSemaphoreGive(devices[i].mutex);
            return true;
        }
    }
    return false;
}

esp_err_t solar_os_sim7670_read_status(const char *name,
                                       sim7670_status_t *status)
{
    solar_os_sim7670_device_t *device = find_device(name);
    if (device == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = sim7670_read_status(&device->modem, status);
    xSemaphoreGive(device->mutex);
    return ret;
}

esp_err_t solar_os_sim7670_command(const char *name,
                                   const char *command,
                                   uint32_t timeout_ms,
                                   char *response,
                                   size_t response_size)
{
    solar_os_sim7670_device_t *device = find_device(name);
    if (device == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = sim7670_command(&device->modem,
                                          command,
                                          timeout_ms,
                                          response,
                                          response_size);
    xSemaphoreGive(device->mutex);
    return ret;
}

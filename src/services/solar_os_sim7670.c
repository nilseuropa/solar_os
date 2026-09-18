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
#include "solar_os_modem.h"

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

static solar_os_modem_network_registration_t map_registration(
    sim7670_registration_t registration)
{
    switch (registration) {
    case SIM7670_REGISTRATION_NOT_REGISTERED:
        return SOLAR_OS_MODEM_REGISTRATION_NOT_REGISTERED;
    case SIM7670_REGISTRATION_SEARCHING:
        return SOLAR_OS_MODEM_REGISTRATION_SEARCHING;
    case SIM7670_REGISTRATION_DENIED:
        return SOLAR_OS_MODEM_REGISTRATION_DENIED;
    case SIM7670_REGISTRATION_HOME:
        return SOLAR_OS_MODEM_REGISTRATION_HOME;
    case SIM7670_REGISTRATION_ROAMING:
        return SOLAR_OS_MODEM_REGISTRATION_ROAMING;
    case SIM7670_REGISTRATION_UNKNOWN:
    default:
        return SOLAR_OS_MODEM_REGISTRATION_UNKNOWN;
    }
}

static esp_err_t modem_get_status(void *ctx, solar_os_modem_status_t *status)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    sim7670_status_t modem_status;
    const esp_err_t ret = sim7670_read_status(&device->modem, &modem_status);
    if (ret == ESP_OK) {
        *status = (solar_os_modem_status_t) {
            .online = modem_status.online,
            .sim_status_valid = modem_status.sim_status_valid,
            .sim_ready = modem_status.sim_ready,
            .registration_status_valid =
                modem_status.registration_status_valid,
            .registration = map_registration(modem_status.registration),
            .signal_status_valid = modem_status.signal_status_valid,
            .rssi_valid = modem_status.rssi_valid,
            .rssi_dbm = modem_status.rssi_dbm,
            .bit_error_rate = modem_status.bit_error_rate,
            .data_status_valid = modem_status.data_status_valid,
            .data_active = modem_status.data_active,
        };
    }
    xSemaphoreGive(device->mutex);
    return ret;
}

static esp_err_t modem_apply_profile(
    void *ctx,
    const solar_os_modem_profile_t *profile)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active || profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sim7670_pdp_type_t pdp_type;
    switch (profile->ip_type) {
    case SOLAR_OS_MODEM_IP_IPV4:
        pdp_type = SIM7670_PDP_IPV4;
        break;
    case SOLAR_OS_MODEM_IP_IPV6:
        pdp_type = SIM7670_PDP_IPV6;
        break;
    case SOLAR_OS_MODEM_IP_IPV4V6:
        pdp_type = SIM7670_PDP_IPV4V6;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    sim7670_auth_t auth;
    switch (profile->auth) {
    case SOLAR_OS_MODEM_AUTH_NONE:
        auth = SIM7670_AUTH_NONE;
        break;
    case SOLAR_OS_MODEM_AUTH_PAP:
        auth = SIM7670_AUTH_PAP;
        break;
    case SOLAR_OS_MODEM_AUTH_CHAP:
        auth = SIM7670_AUTH_CHAP;
        break;
    case SOLAR_OS_MODEM_AUTH_AUTO:
        auth = SIM7670_AUTH_AUTO;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = sim7670_configure_pdp(&device->modem,
                                                profile->apn,
                                                pdp_type,
                                                auth,
                                                profile->username,
                                                profile->password);
    xSemaphoreGive(device->mutex);
    return ret;
}

static esp_err_t modem_clear_profile(void *ctx)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = sim7670_clear_pdp(&device->modem);
    xSemaphoreGive(device->mutex);
    return ret;
}

static esp_err_t modem_set_data_active(void *ctx, bool active)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = sim7670_set_pdp_active(&device->modem, active);
    xSemaphoreGive(device->mutex);
    return ret;
}

static esp_err_t modem_unlock_sim(void *ctx, const char *pin)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = sim7670_unlock_sim(&device->modem, pin);
    xSemaphoreGive(device->mutex);
    return ret;
}

static esp_err_t modem_command(void *ctx,
                               const char *command,
                               uint32_t timeout_ms,
                               char *response,
                               size_t response_size)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_ARG;
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

static const solar_os_modem_ops_t modem_ops = {
    .get_status = modem_get_status,
    .apply_profile = modem_apply_profile,
    .clear_profile = modem_clear_profile,
    .set_data_active = modem_set_data_active,
    .unlock_sim = modem_unlock_sim,
    .command = modem_command,
};

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

    const solar_os_modem_registration_t modem_registration = {
        .name = name,
        .driver = "sim7670",
        .transport = device->uart_bus,
        .ops = &modem_ops,
        .ctx = device,
    };
    ret = solar_os_modem_register(&modem_registration);
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
        (void)solar_os_modem_unregister(name);
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
    ESP_RETURN_ON_ERROR(solar_os_modem_unregister(name),
                        TAG,
                        "modem unregister failed");
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

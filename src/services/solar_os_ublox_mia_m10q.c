#include "solar_os_ublox_mia_m10q.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_buses.h"
#include "solar_os_gnss.h"
#include "solar_os_gpio_controller.h"
#include "ubx.h"

#define UBLOX_DEVICE_MAX 2U
#define UBX_CLASS_NAV 0x01U
#define UBX_ID_NAV_PVT 0x07U
#define UBX_CLASS_MON 0x0AU
#define UBX_ID_MON_VER 0x04U
#define UBX_NAV_PVT_LENGTH 92U
#define UBX_POLL_RETRY_MS 500U
#define UBLOX_POWER_ON_SETTLE_MS 100U
#define UBLOX_POWER_ON_TIMEOUT_MS 5000U

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char uart_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    bool power_control;
    bool powered;
    solar_os_gpio_line_ref_t power_line;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
} ublox_device_t;

static const char *TAG = "ublox-mia-m10q";
static ublox_device_t devices[UBLOX_DEVICE_MAX];

static uint16_t read_u16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
}
static uint32_t read_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8U) |
        ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
}

static int32_t read_i32(const uint8_t *data)
{
    return (int32_t)read_u32(data);
}

static esp_err_t poll_message(ublox_device_t *device,
                              uint8_t message_class,
                              uint8_t message_id,
                              uint32_t timeout_ms,
                              ubx_parser_t *response)
{
    uint8_t poll[8];
    const size_t poll_len = ubx_encode_poll(message_class, message_id, poll);
    ubx_parser_t parser;
    ubx_parser_reset(&parser);
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
    while (esp_timer_get_time() < deadline) {
        size_t written = 0U;
        ESP_RETURN_ON_ERROR(solar_os_bus_uart_write(device->uart_bus,
                                                     poll,
                                                     poll_len,
                                                     &written),
                            TAG,
                            "UBX poll write failed");
        if (written != poll_len) {
            return ESP_ERR_INVALID_SIZE;
        }

        int64_t retry_deadline =
            esp_timer_get_time() + (int64_t)UBX_POLL_RETRY_MS * 1000LL;
        if (retry_deadline > deadline) {
            retry_deadline = deadline;
        }
        while (esp_timer_get_time() < retry_deadline) {
            uint8_t data[64];
            size_t read_len = 0U;
            const int64_t remaining_us = retry_deadline - esp_timer_get_time();
            const uint32_t wait_ms = remaining_us > 20000LL
                ? 20U
                : (uint32_t)((remaining_us + 999LL) / 1000LL);
            ESP_RETURN_ON_ERROR(solar_os_bus_uart_read(device->uart_bus,
                                                        data,
                                                        sizeof(data),
                                                        wait_ms,
                                                        &read_len),
                                TAG,
                                "UBX read failed");
            for (size_t i = 0; i < read_len; i++) {
                if (ubx_parser_feed(&parser, data[i]) &&
                    parser.message_class == message_class &&
                    parser.message_id == message_id) {
                    *response = parser;
                    return ESP_OK;
                }
            }
        }
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t read_fix(void *ctx,
                          uint32_t timeout_ms,
                          solar_os_gnss_fix_t *fix)
{
    ublox_device_t *device = ctx;
    if (device == NULL || !device->active || fix == NULL || timeout_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    if (!device->powered) {
        xSemaphoreGive(device->mutex);
        return ESP_ERR_INVALID_STATE;
    }
    ubx_parser_t response;
    esp_err_t ret = poll_message(device,
                                 UBX_CLASS_NAV,
                                 UBX_ID_NAV_PVT,
                                 timeout_ms,
                                 &response);
    if (ret != ESP_OK) {
        xSemaphoreGive(device->mutex);
        ESP_LOGE(TAG, "NAV-PVT poll failed: %s", esp_err_to_name(ret));
        return ret;
    }
    if (response.payload_len != UBX_NAV_PVT_LENGTH) {
        xSemaphoreGive(device->mutex);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const uint8_t *p = response.payload;
    *fix = (solar_os_gnss_fix_t) {
        .valid = p[20] >= 2U && (p[21] & 0x01U) != 0U,
        .time_valid = (p[11] & 0x03U) == 0x03U,
        .year = read_u16(&p[4]),
        .month = p[6],
        .day = p[7],
        .hour = p[8],
        .minute = p[9],
        .second = p[10],
        .fix_type = p[20],
        .satellites = p[23],
        .longitude_deg_e7 = read_i32(&p[24]),
        .latitude_deg_e7 = read_i32(&p[28]),
        .height_msl_mm = read_i32(&p[36]),
        .horizontal_accuracy_mm = read_u32(&p[40]),
        .vertical_accuracy_mm = read_u32(&p[44]),
        .ground_speed_mm_s = read_i32(&p[60]),
        .heading_deg_e5 = read_i32(&p[64]),
        .position_dop_e2 = read_u16(&p[76]),
    };
    xSemaphoreGive(device->mutex);
    return ESP_OK;
}

static esp_err_t set_power(void *ctx, bool enabled)
{
    ublox_device_t *device = ctx;
    if (device == NULL || !device->active || !device->power_control) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    if (device->powered == enabled) {
        xSemaphoreGive(device->mutex);
        return ESP_OK;
    }
    esp_err_t ret = solar_os_gpio_line_write(&device->power_line, enabled);
    if (ret == ESP_OK && enabled) {
        vTaskDelay(pdMS_TO_TICKS(UBLOX_POWER_ON_SETTLE_MS));
        ubx_parser_t version;
        ret = poll_message(device,
                           UBX_CLASS_MON,
                           UBX_ID_MON_VER,
                           UBLOX_POWER_ON_TIMEOUT_MS,
                           &version);
        if (ret != ESP_OK) {
            (void)solar_os_gpio_line_write(&device->power_line, false);
        }
    }
    if (ret == ESP_OK) {
        device->powered = enabled;
    }
    xSemaphoreGive(device->mutex);
    return ret;
}

static const solar_os_gnss_ops_t gnss_ops = {
    .read_fix = read_fix,
};

static const solar_os_gnss_ops_t powered_gnss_ops = {
    .read_fix = read_fix,
    .set_power = set_power,
};

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *uart_bus,
                                size_t uart_bus_len,
                                bool *power_control,
                                solar_os_gpio_line_ref_t *power_line)
{
    bool have_uart = false;
    *power_control = false;
    for (size_t i = 0; bindings != NULL && i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_UART_PORT && !have_uart) {
            strlcpy(uart_bus, binding->target, uart_bus_len);
            have_uart = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO_LINE &&
                   strcmp(binding->role, "power") == 0 && !*power_control) {
            strlcpy(power_line->controller,
                    binding->target,
                    sizeof(power_line->controller));
            power_line->line = (uint8_t)binding->value;
            *power_control = true;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return have_uart && solar_os_expansion_find_uart_port(uart_bus, NULL, NULL)
        ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t solar_os_ublox_mia_m10q_attach(
    const char *name,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    ublox_device_t *device = NULL;
    for (size_t i = 0; i < UBLOX_DEVICE_MAX; i++) {
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
    bool power_control = false;
    solar_os_gpio_line_ref_t power_line = {0};
    ESP_RETURN_ON_ERROR(parse_bindings(bindings,
                                       binding_count,
                                       uart_bus,
                                       sizeof(uart_bus),
                                       &power_control,
                                       &power_line),
                        TAG,
                        "invalid bindings");
    memset(device, 0, sizeof(*device));
    device->active = true;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->uart_bus, uart_bus, sizeof(device->uart_bus));
    device->power_control = power_control;
    device->power_line = power_line;
    device->mutex = xSemaphoreCreateMutexStatic(&device->mutex_storage);
    if (device->mutex == NULL) {
        memset(device, 0, sizeof(*device));
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_OK;
    if (power_control) {
        ret = solar_os_gpio_line_write(&device->power_line, false);
    } else {
        ubx_parser_t version;
        ret = poll_message(device, UBX_CLASS_MON, UBX_ID_MON_VER, 500U, &version);
        device->powered = ret == ESP_OK;
    }
    if (ret != ESP_OK) {
        memset(device, 0, sizeof(*device));
        return ret;
    }
    const solar_os_gnss_registration_t registration = {
        .name = name,
        .driver = "ublox-mia-m10q",
        .ops = power_control ? &powered_gnss_ops : &gnss_ops,
        .ctx = device,
        .powered = device->powered,
    };
    ret = solar_os_gnss_register(&registration);
    if (ret != ESP_OK) {
        memset(device, 0, sizeof(*device));
        return ret;
    }
    ESP_LOGI(TAG,
             "%s attached on %s power=%s",
             name,
             uart_bus,
             power_control ? "off" : "always-on");
    return ESP_OK;
}

esp_err_t solar_os_ublox_mia_m10q_detach(const char *name)
{
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < UBLOX_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            ESP_RETURN_ON_ERROR(solar_os_gnss_unregister(name), TAG, "unregister failed");
            if (devices[i].power_control) {
                (void)solar_os_gpio_line_write(&devices[i].power_line, false);
            }
            memset(&devices[i], 0, sizeof(devices[i]));
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

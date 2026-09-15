#include "solar_os_ublox_mia_m10q.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "solar_os_buses.h"
#include "solar_os_gnss.h"
#include "ubx.h"

#define UBLOX_DEVICE_MAX 2U
#define UBX_CLASS_NAV 0x01U
#define UBX_ID_NAV_PVT 0x07U
#define UBX_CLASS_MON 0x0AU
#define UBX_ID_MON_VER 0x04U
#define UBX_NAV_PVT_LENGTH 92U

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char uart_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
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

    ubx_parser_t parser;
    ubx_parser_reset(&parser);
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
    while (esp_timer_get_time() < deadline) {
        uint8_t data[64];
        size_t read_len = 0U;
        const int64_t remaining_us = deadline - esp_timer_get_time();
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
    ubx_parser_t response;
    ESP_RETURN_ON_ERROR(poll_message(device,
                                     UBX_CLASS_NAV,
                                     UBX_ID_NAV_PVT,
                                     timeout_ms,
                                     &response),
                        TAG,
                        "NAV-PVT poll failed");
    if (response.payload_len != UBX_NAV_PVT_LENGTH) {
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
    return ESP_OK;
}

static const solar_os_gnss_ops_t gnss_ops = {
    .read_fix = read_fix,
};

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *uart_bus,
                                size_t uart_bus_len)
{
    if (bindings == NULL || uart_bus == NULL || binding_count != 1U ||
        bindings[0].kind != SOLAR_OS_EXPANSION_BINDING_UART_PORT ||
        bindings[0].target[0] == '\0' ||
        !solar_os_expansion_find_uart_port(bindings[0].target, NULL, NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(uart_bus, bindings[0].target, uart_bus_len);
    return ESP_OK;
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

    ubx_parser_t version;
    esp_err_t ret = poll_message(device, UBX_CLASS_MON, UBX_ID_MON_VER, 500U, &version);
    if (ret != ESP_OK) {
        memset(device, 0, sizeof(*device));
        return ret;
    }
    const solar_os_gnss_registration_t registration = {
        .name = name,
        .driver = "ublox-mia-m10q",
        .ops = &gnss_ops,
        .ctx = device,
    };
    ret = solar_os_gnss_register(&registration);
    if (ret != ESP_OK) {
        memset(device, 0, sizeof(*device));
        return ret;
    }
    ESP_LOGI(TAG, "%s attached on %s", name, uart_bus);
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
            memset(&devices[i], 0, sizeof(devices[i]));
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

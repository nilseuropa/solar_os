#include "solar_os_nmea_gnss.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nmea.h"
#include "solar_os_buses.h"
#include "solar_os_gnss.h"
#include "solar_os_gpio_controller.h"

/*
 * Generic NMEA 0183 GNSS receiver on a UART bus. The receiver streams RMC
 * and GGA unprompted, so the driver only listens and never configures it.
 * Power-on detection tries both common factory baud rates (9600 and 115200)
 * and both enable-rail polarities.
 *
 * While the receiver is powered, a reader task drains the stream and keeps
 * the latest sentences with their arrival times, and fix requests are
 * answered from those. Reading only on demand would leave stale sentences in
 * the RX buffer.
 */

#define NMEA_GNSS_DEVICE_MAX 2U
#define NMEA_GNSS_POWER_ON_SETTLE_MS 300U
#define NMEA_GNSS_DETECT_TIMEOUT_MS 2500U
#define NMEA_GNSS_PROBE_TIMEOUT_MS 1500U
#define NMEA_GNSS_READER_STACK 4096U
#define NMEA_GNSS_READER_PRIORITY 3U
#define NMEA_GNSS_READ_SLICE_MS 50U
#define NMEA_GNSS_FRESH_US 2500000LL

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char uart_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    bool power_control;
    bool powered;
    bool power_active_high;
    uint32_t alternate_baud;
    uint32_t current_baud;
    solar_os_gpio_line_ref_t power_line;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
    volatile bool reader_run;
    volatile bool reader_exited;
    portMUX_TYPE snapshot_lock;
    nmea_fix_state_t snapshot;
    int64_t rmc_us;
    int64_t gga_us;
    uint32_t bytes;
} nmea_gnss_device_t;

static const char *TAG = "nmea-gnss";
static nmea_gnss_device_t devices[NMEA_GNSS_DEVICE_MAX];

/* Waits for one checksum-valid RMC or GGA sentence. */
static esp_err_t await_sentence(nmea_gnss_device_t *device, uint32_t timeout_ms)
{
    nmea_parser_t parser;
    nmea_fix_state_t state;
    nmea_parser_reset(&parser);
    nmea_fix_state_reset(&state);
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
                            "UART read failed");
        for (size_t i = 0; i < read_len; i++) {
            if (nmea_parser_feed(&parser, data[i], &state)) {
                return ESP_OK;
            }
        }
    }
    return ESP_ERR_TIMEOUT;
}

/* Discards bytes received before power-on, so a stale sentence cannot pass
 * detection. */
static void drain_rx(nmea_gnss_device_t *device)
{
    for (unsigned i = 0; i < 128U; i++) {
        uint8_t data[64];
        size_t read_len = 0U;
        if (solar_os_bus_uart_read(device->uart_bus, data, sizeof(data), 1U, &read_len) != ESP_OK ||
            read_len == 0U) {
            return;
        }
    }
}

/* Waits for a sentence at the current baud rate, then at the other one.
 * Leaves the working rate applied, or the starting rate on failure. */
static esp_err_t detect_stream(nmea_gnss_device_t *device, uint32_t timeout_ms)
{
    esp_err_t ret = await_sentence(device, timeout_ms);
    if (ret == ESP_OK || device->alternate_baud == 0U) {
        return ret;
    }
    const uint32_t start_baud = device->current_baud;
    const uint32_t other_baud = start_baud == device->alternate_baud
        ? SOLAR_OS_BUS_UART_DEFAULT_BAUD_RATE
        : device->alternate_baud;
    ESP_RETURN_ON_ERROR(solar_os_bus_uart_set_baud_rate(device->uart_bus,
                                                        other_baud,
                                                        device->name),
                        TAG,
                        "alternate baud switch failed");
    device->current_baud = other_baud;
    if (await_sentence(device, timeout_ms) == ESP_OK) {
        ESP_LOGW(TAG,
                 "%s streams at %lu baud",
                 device->name,
                 (unsigned long)other_baud);
        return ESP_OK;
    }
    if (solar_os_bus_uart_set_baud_rate(device->uart_bus, start_baud, device->name) == ESP_OK) {
        device->current_baud = start_baud;
    }
    return ESP_ERR_TIMEOUT;
}

static void reader_main(void *arg)
{
    nmea_gnss_device_t *device = arg;
    nmea_parser_t parser;
    nmea_fix_state_t state;
    nmea_parser_reset(&parser);
    nmea_fix_state_reset(&state);
    while (device->reader_run) {
        uint8_t data[64];
        size_t read_len = 0U;
        if (solar_os_bus_uart_read(device->uart_bus,
                                   data,
                                   sizeof(data),
                                   NMEA_GNSS_READ_SLICE_MS,
                                   &read_len) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        for (size_t i = 0; i < read_len; i++) {
            const uint32_t rmc_before = state.rmc_count;
            const uint32_t gga_before = state.gga_count;
            if (!nmea_parser_feed(&parser, data[i], &state)) {
                continue;
            }
            const int64_t now = esp_timer_get_time();
            portENTER_CRITICAL(&device->snapshot_lock);
            device->snapshot = state;
            if (state.rmc_count != rmc_before) {
                device->rmc_us = now;
            }
            if (state.gga_count != gga_before) {
                device->gga_us = now;
            }
            portEXIT_CRITICAL(&device->snapshot_lock);
        }
        portENTER_CRITICAL(&device->snapshot_lock);
        device->bytes += (uint32_t)read_len;
        portEXIT_CRITICAL(&device->snapshot_lock);
    }
    device->reader_exited = true;
    vTaskDelete(NULL);
}

static esp_err_t reader_start(nmea_gnss_device_t *device)
{
    if (device->reader_run) {
        return ESP_OK;
    }
    nmea_fix_state_reset(&device->snapshot);
    device->rmc_us = 0;
    device->gga_us = 0;
    device->bytes = 0U;
    device->reader_exited = false;
    device->reader_run = true;
    if (xTaskCreate(reader_main,
                    "nmea_gnss",
                    NMEA_GNSS_READER_STACK,
                    device,
                    NMEA_GNSS_READER_PRIORITY,
                    NULL) != pdPASS) {
        device->reader_run = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void reader_stop(nmea_gnss_device_t *device)
{
    if (!device->reader_run) {
        return;
    }
    device->reader_run = false;
    while (!device->reader_exited) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static esp_err_t write_power_level(nmea_gnss_device_t *device, bool enabled)
{
    const bool level = enabled == device->power_active_high;
    return solar_os_gpio_line_write(&device->power_line, level);
}

static esp_err_t read_fix(void *ctx,
                          uint32_t timeout_ms,
                          solar_os_gnss_fix_t *fix)
{
    nmea_gnss_device_t *device = ctx;
    if (device == NULL || !device->active || fix == NULL || timeout_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    if (!device->powered) {
        xSemaphoreGive(device->mutex);
        return ESP_ERR_INVALID_STATE;
    }
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
    nmea_fix_state_t state;
    bool gga_fresh = false;
    uint32_t bytes = 0U;
    for (;;) {
        const int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&device->snapshot_lock);
        state = device->snapshot;
        const bool rmc_fresh = device->rmc_us != 0 &&
            now - device->rmc_us <= NMEA_GNSS_FRESH_US;
        gga_fresh = device->gga_us != 0 &&
            now - device->gga_us <= NMEA_GNSS_FRESH_US;
        bytes = device->bytes;
        portEXIT_CRITICAL(&device->snapshot_lock);
        if (rmc_fresh) {
            break;
        }
        if (now >= deadline) {
            xSemaphoreGive(device->mutex);
            ESP_LOGW(TAG,
                     "%s: no RMC within %lu ms (%lu bytes, %lu RMC, %lu GGA seen)",
                     device->name,
                     (unsigned long)timeout_ms,
                     (unsigned long)bytes,
                     (unsigned long)state.rmc_count,
                     (unsigned long)state.gga_count);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    xSemaphoreGive(device->mutex);
    *fix = (solar_os_gnss_fix_t) {
        .valid = state.valid,
        .time_valid = state.date_valid,
        .satellites_valid = gga_fresh,
        .year = state.year,
        .month = state.month,
        .day = state.day,
        .hour = state.hour,
        .minute = state.minute,
        .second = state.second,
        .fix_type = state.valid ? (state.altitude_valid ? 3U : 2U) : 0U,
        .satellites = gga_fresh ? state.satellites : 0U,
        .latitude_deg_e7 = state.latitude_deg_e7,
        .longitude_deg_e7 = state.longitude_deg_e7,
        .height_msl_mm = state.altitude_msl_mm,
        /* RMC/GGA carry no accuracy estimate; approximate five meters of
         * horizontal error per unit of HDOP. */
        .horizontal_accuracy_mm = (uint32_t)state.hdop_e2 * 50U,
        .ground_speed_mm_s = state.ground_speed_mm_s,
        .heading_deg_e5 = state.course_deg_e5,
        .position_dop_e2 = state.hdop_e2,
    };
    return ESP_OK;
}

static esp_err_t set_power(void *ctx, bool enabled)
{
    nmea_gnss_device_t *device = ctx;
    if (device == NULL || !device->active || !device->power_control) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    if (device->powered == enabled) {
        xSemaphoreGive(device->mutex);
        return ESP_OK;
    }
    if (!enabled) {
        reader_stop(device);
    }
    esp_err_t ret = write_power_level(device, enabled);
    if (ret == ESP_OK && enabled) {
        vTaskDelay(pdMS_TO_TICKS(NMEA_GNSS_POWER_ON_SETTLE_MS));
        drain_rx(device);
        ret = detect_stream(device, NMEA_GNSS_DETECT_TIMEOUT_MS);
        if (ret != ESP_OK) {
            /* Retry with the opposite enable polarity, and keep whichever
             * works. */
            if (write_power_level(device, !enabled) == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(NMEA_GNSS_POWER_ON_SETTLE_MS));
                drain_rx(device);
                ret = detect_stream(device, NMEA_GNSS_DETECT_TIMEOUT_MS);
            }
            if (ret == ESP_OK) {
                device->power_active_high = !device->power_active_high;
                ESP_LOGW(TAG,
                         "%s power enable is active-%s on this board revision",
                         device->name,
                         device->power_active_high ? "high" : "low");
            } else {
                (void)write_power_level(device, false);
            }
        }
        if (ret == ESP_OK) {
            ret = reader_start(device);
            if (ret != ESP_OK) {
                (void)write_power_level(device, false);
            }
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
                                solar_os_gpio_line_ref_t *power_line,
                                bool *power_active_high,
                                uint32_t *alternate_baud)
{
    bool have_uart = false;
    *power_control = false;
    *power_active_high = true;
    *alternate_baud = 0U;
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
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   strcmp(binding->role, "active") == 0) {
            *power_active_high = binding->value != 0;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   strcmp(binding->role, "alt_baud") == 0) {
            *alternate_baud = (uint32_t)binding->value;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return have_uart && solar_os_expansion_find_uart_port(uart_bus, NULL, NULL)
        ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t solar_os_nmea_gnss_attach(const char *name,
                                    const solar_os_expansion_binding_t *bindings,
                                    size_t binding_count)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    nmea_gnss_device_t *device = NULL;
    for (size_t i = 0; i < NMEA_GNSS_DEVICE_MAX; i++) {
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
    bool power_control;
    bool power_active_high;
    uint32_t alternate_baud;
    solar_os_gpio_line_ref_t power_line = {0};
    ESP_RETURN_ON_ERROR(parse_bindings(bindings,
                                       binding_count,
                                       uart_bus,
                                       sizeof(uart_bus),
                                       &power_control,
                                       &power_line,
                                       &power_active_high,
                                       &alternate_baud),
                        TAG,
                        "invalid bindings");
    memset(device, 0, sizeof(*device));
    device->active = true;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->uart_bus, uart_bus, sizeof(device->uart_bus));
    device->power_control = power_control;
    device->power_active_high = power_active_high;
    device->alternate_baud = alternate_baud;
    device->current_baud = SOLAR_OS_BUS_UART_DEFAULT_BAUD_RATE;
    device->power_line = power_line;
    portMUX_INITIALIZE(&device->snapshot_lock);
    device->mutex = xSemaphoreCreateMutexStatic(&device->mutex_storage);
    if (device->mutex == NULL) {
        memset(device, 0, sizeof(*device));
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret;
    if (power_control) {
        ret = write_power_level(device, false);
    } else {
        ret = detect_stream(device, NMEA_GNSS_PROBE_TIMEOUT_MS);
        if (ret == ESP_OK) {
            ret = reader_start(device);
        }
        device->powered = ret == ESP_OK;
    }
    if (ret != ESP_OK) {
        memset(device, 0, sizeof(*device));
        return ret;
    }
    const solar_os_gnss_registration_t registration = {
        .name = name,
        .driver = "nmea",
        .ops = power_control ? &powered_gnss_ops : &gnss_ops,
        .ctx = device,
        .powered = device->powered,
    };
    ret = solar_os_gnss_register(&registration);
    if (ret != ESP_OK) {
        reader_stop(device);
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

esp_err_t solar_os_nmea_gnss_detach(const char *name)
{
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < NMEA_GNSS_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            ESP_RETURN_ON_ERROR(solar_os_gnss_unregister(name),
                                TAG,
                                "unregister failed");
            reader_stop(&devices[i]);
            if (devices[i].power_control) {
                (void)write_power_level(&devices[i], false);
            }
            memset(&devices[i], 0, sizeof(devices[i]));
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

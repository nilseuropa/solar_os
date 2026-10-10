#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"

/* Model task creation and exit without running a UART reader thread. */
#define portMUX_INITIALIZE(lock) (*(lock) = 0)
#define pdPASS 1
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack,
                void *arg, unsigned priority, void *handle);
void vTaskDelete(void *handle);

static void test_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}
#undef ESP_LOGW
#define ESP_LOGW(...) test_log(__VA_ARGS__)

#include "../../src/services/solar_os_nmea_gnss.c"

static int64_t now_us = 10000000;
static bool hardware_power;
static bool active_high = true;
static bool fail_off;
static bool fail_on;
static bool stream_available;
static bool fail_task_create;
static unsigned power_writes;
static unsigned reader_starts;
static uint32_t baud_changes[8];
static size_t baud_change_count;

int64_t esp_timer_get_time(void) { return now_us; }

void vTaskDelay(TickType_t ticks)
{
    now_us += (int64_t)ticks * 1000;
    for (size_t i = 0; i < NMEA_GNSS_DEVICE_MAX; i++) {
        if (!devices[i].reader_run) {
            devices[i].reader_exited = true;
        }
    }
}

void vTaskDelete(void *handle) { (void)handle; }

int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack,
                void *arg, unsigned priority, void *handle)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)handle;
    if (fail_task_create) {
        return pdFALSE;
    }
    reader_starts++;
    return pdPASS;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t len = strlen(src);
    if (size != 0) {
        const size_t copy = len < size - 1 ? len : size - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

bool solar_os_expansion_find_uart_port(const char *name,
                                      solar_os_expansion_uart_port_t *port,
                                      size_t *index)
{
    (void)port; (void)index;
    return strcmp(name, "uart0") == 0;
}

bool solar_os_bus_find(const char *name, solar_os_bus_protocol_t protocol,
                       solar_os_bus_info_t *info)
{
    assert(strcmp(name, "uart0") == 0 && protocol == SOLAR_OS_BUS_PROTOCOL_UART);
    *info = (solar_os_bus_info_t) {0};
    info->config.uart.baud_rate = 9600;
    return true;
}

esp_err_t solar_os_gpio_line_write(const solar_os_gpio_line_ref_t *line, bool level)
{
    (void)line;
    power_writes++;
    const bool enabled = level == active_high;
    if ((enabled && fail_on) || (!enabled && fail_off)) {
        return ESP_FAIL;
    }
    hardware_power = enabled;
    return ESP_OK;
}

esp_err_t solar_os_bus_uart_read(const char *name, uint8_t *data, size_t len,
                                 uint32_t timeout_ms, size_t *read_len)
{
    assert(strcmp(name, "uart0") == 0);
    now_us += (int64_t)timeout_ms * 1000;
    *read_len = 0;
    if (stream_available && timeout_ms > 1) {
        const char *line = "$GNRMC,120000.00,V,,,,,,,010126,,,N*64\r\n";
        assert(strlen(line) <= len);
        *read_len = strlen(line);
        memcpy(data, line, *read_len);
    }
    return ESP_OK;
}

esp_err_t solar_os_bus_uart_set_baud_rate(const char *name, uint32_t baud,
                                          const char *owner)
{
    assert(strcmp(name, "uart0") == 0 && strcmp(owner, "gnss0") == 0);
    assert(baud_change_count < sizeof(baud_changes) / sizeof(baud_changes[0]));
    baud_changes[baud_change_count++] = baud;
    return ESP_OK;
}

static nmea_gnss_device_t *attach(bool controlled)
{
    assert(solar_os_gnss_count() == 0);
    hardware_power = false;
    fail_on = fail_off = fail_task_create = false;
    stream_available = !controlled;
    power_writes = reader_starts = 0;
    baud_change_count = 0;
    const solar_os_expansion_binding_t bindings[] = {
        {.kind = SOLAR_OS_EXPANSION_BINDING_UART_PORT, .target = "uart0"},
        {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE, .role = "power", .target = "gpio", .value = 2},
        {.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "active", .value = active_high},
        {.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "alt_baud", .value = 115200},
    };
    assert(solar_os_nmea_gnss_attach("gnss0", bindings, controlled ? 4 : 1) == ESP_OK);
    return &devices[0];
}

static void assert_power(bool powered)
{
    solar_os_gnss_info_t info;
    assert(solar_os_gnss_get(0, &info));
    assert(info.powered == powered && devices[0].powered == powered);
}

static void test_startup_failure_and_recovery(void)
{
    for (unsigned polarity = 0; polarity < 2; polarity++) {
        active_high = polarity != 0;
        nmea_gnss_device_t *device = attach(true);
        fail_off = true;
        assert(solar_os_gnss_set_power("gnss0", true) == ESP_ERR_TIMEOUT);
        assert(hardware_power && !device->reader_run);
        assert_power(true);
        solar_os_gnss_status_t status;
        assert(solar_os_gnss_get_status("gnss0", 1, &status) == ESP_ERR_TIMEOUT);
        assert(status.powered && !status.fix_available);

        /* A failed retry of the on write must preserve the known state. */
        fail_on = true;
        assert(solar_os_gnss_set_power("gnss0", true) == ESP_FAIL);
        assert_power(true);
        fail_on = fail_off = false;
        assert(solar_os_gnss_set_power("gnss0", true) == ESP_ERR_TIMEOUT);
        assert(!hardware_power);
        assert_power(false);

        stream_available = true;
        fail_task_create = fail_off = true;
        assert(solar_os_gnss_set_power("gnss0", true) == ESP_ERR_NO_MEM);
        assert(hardware_power && !device->reader_run);
        assert_power(true);
        fail_task_create = fail_off = false;
        const unsigned before = power_writes;
        assert(solar_os_gnss_set_power("gnss0", false) == ESP_OK);
        assert(!hardware_power && power_writes == before + 1);
        assert_power(false);
        assert(solar_os_gnss_set_power("gnss0", true) == ESP_OK);
        assert(device->reader_run && reader_starts == 1);
        fail_off = true;
        assert(solar_os_gnss_set_power("gnss0", false) == ESP_FAIL);
        assert(device->reader_run && hardware_power);
        assert_power(true);
        assert(solar_os_gnss_set_power("gnss0", true) == ESP_OK);
        assert(reader_starts == 1);
        fail_off = false;
        assert(solar_os_nmea_gnss_detach("gnss0") == ESP_OK);
        assert(!hardware_power && solar_os_gnss_count() == 0);
    }
}

static void test_failed_detach_retains_provider(void)
{
    active_high = true;
    nmea_gnss_device_t *device = attach(true);
    stream_available = true;
    assert(solar_os_gnss_set_power("gnss0", true) == ESP_OK);
    fail_off = true;
    assert(solar_os_nmea_gnss_detach("gnss0") == ESP_FAIL);
    assert(device->active && device->reader_run && hardware_power);
    assert_power(true);
    assert(solar_os_gnss_set_power("gnss0", true) == ESP_OK);
    assert(reader_starts == 1);
    fail_off = false;
    assert(solar_os_nmea_gnss_detach("gnss0") == ESP_OK);
    assert(!hardware_power && !device->active && !device->reader_run);
    assert(solar_os_gnss_count() == 0);
    /* The slot remains reusable after the successful retry. */
    attach(true);
    assert(solar_os_nmea_gnss_detach("gnss0") == ESP_OK);
}

static void feed_snapshot(nmea_gnss_device_t *device, const char *body)
{
    nmea_parser_t parser;
    nmea_parser_reset(&parser);
    uint8_t checksum = 0;
    for (const char *p = body; *p; p++) checksum ^= (uint8_t)*p;
    char line[120];
    snprintf(line, sizeof(line), "$%s*%02X\r\n", body, checksum);
    bool accepted = false;
    for (const char *p = line; *p; p++) {
        accepted |= nmea_parser_feed(&parser, (uint8_t)*p, &device->snapshot);
    }
    assert(accepted);
}

static void test_fix_metadata_and_freshness(void)
{
    nmea_gnss_device_t *device = attach(false);
    assert(device->reader_run && reader_starts == 1);
    feed_snapshot(device, "GPRMC,120000,A,4500.00,N,01000.00,E,0,0,061026,,,A");
    feed_snapshot(device, "GPGGA,120000,4500.00,N,01000.00,E,1,08,0.94,545.4,M,0.0,M,,");
    device->rmc_us = device->gga_us = now_us;
    solar_os_gnss_fix_t fix;
    assert(solar_os_gnss_read_fix("gnss0", 100, &fix) == ESP_OK);
    assert(fix.valid && fix.time_valid && fix.fix_type == 3 && fix.satellites_valid);
    assert(fix.satellites == 8 && fix.height_msl_mm == 545400);
    assert(device->snapshot.hdop_e2 == 94);
    assert(fix.horizontal_accuracy_mm == 0 && fix.vertical_accuracy_mm == 0 && fix.position_dop_e2 == 0);
    device->gga_us = now_us - NMEA_GNSS_FRESH_US - 1;
    assert(solar_os_gnss_read_fix("gnss0", 100, &fix) == ESP_OK);
    assert(fix.valid && fix.fix_type == 2 && !fix.satellites_valid && fix.height_msl_mm == 0);
    feed_snapshot(device, "GPGGA,120000,4500.00,N,01000.00,E,0,00,99.99,545.4,M,0.0,M,,");
    device->gga_us = now_us;
    assert(solar_os_gnss_read_fix("gnss0", 100, &fix) == ESP_OK);
    assert(fix.valid && fix.fix_type == 2 && fix.height_msl_mm == 0);
    feed_snapshot(device, "GPRMC,120000junk,A,4500.00,N,01000.00,E,0,0,310226,,,A");
    assert(solar_os_gnss_read_fix("gnss0", 100, &fix) == ESP_OK);
    assert(fix.valid && !fix.time_valid);
    device->rmc_us = now_us - NMEA_GNSS_FRESH_US - 1;
    assert(solar_os_gnss_read_fix("gnss0", 1, &fix) == ESP_ERR_TIMEOUT);
    const unsigned before = power_writes;
    assert(solar_os_nmea_gnss_detach("gnss0") == ESP_OK);
    assert(power_writes == before && solar_os_gnss_count() == 0);
}

static void test_configured_baud_restoration(void)
{
    nmea_gnss_device_t *device = attach(true);
    assert(solar_os_gnss_set_power("gnss0", true) == ESP_ERR_TIMEOUT);
    assert(baud_change_count == 2 && baud_changes[0] == 115200 && baud_changes[1] == 9600);
    assert(device->current_baud == 9600);
    assert_power(false);
    assert(solar_os_nmea_gnss_detach("gnss0") == ESP_OK);
}

static esp_err_t detach_during_read(void *ctx, uint32_t timeout_ms,
                                    solar_os_gnss_fix_t *fix)
{
    (void)ctx; (void)timeout_ms; (void)fix;
    /* The real registry pins the provider for this callback. */
    assert(solar_os_nmea_gnss_detach("gnss0") == ESP_ERR_INVALID_STATE);
    assert(devices[0].active && !devices[0].reader_run && !hardware_power);
    assert_power(false);
    return ESP_ERR_INVALID_STATE;
}

static void test_busy_unregister_retains_device(void)
{
    nmea_gnss_device_t *device = attach(true);
    stream_available = true;
    assert(solar_os_gnss_set_power("gnss0", true) == ESP_OK);
    assert(solar_os_gnss_unregister("gnss0") == ESP_OK);
    const solar_os_gnss_ops_t ops = {
        .read_fix = detach_during_read, .set_power = set_power,
    };
    const solar_os_gnss_registration_t registration = {
        .name = "gnss0", .driver = "nmea", .ops = &ops, .ctx = device, .powered = true,
    };
    assert(solar_os_gnss_register(&registration) == ESP_OK);
    solar_os_gnss_fix_t fix;
    assert(solar_os_gnss_read_fix("gnss0", 100, &fix) == ESP_ERR_INVALID_STATE);
    assert(solar_os_gnss_set_power("gnss0", true) == ESP_OK);
    assert(device->reader_run && hardware_power && reader_starts == 2);
    assert(solar_os_nmea_gnss_detach("gnss0") == ESP_OK);
    assert(!hardware_power && solar_os_gnss_count() == 0);
}

int main(void)
{
    test_startup_failure_and_recovery();
    test_failed_detach_retains_provider();
    test_fix_metadata_and_freshness();
    test_configured_baud_restoration();
    test_busy_unregister_retains_device();
    puts("NMEA GNSS service tests: ok");
    return 0;
}

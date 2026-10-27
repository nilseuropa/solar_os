#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Capture the real shared transport and the controller-specific backend. */
#include "../../src/drivers/epd_ultrachip.c"
#undef TRY
#include "../../src/drivers/epd_uc8279.c"

typedef struct {
    uint8_t command;
    size_t length;
    uint8_t data[60000];
} capture_t;
static capture_t captures[80];
static size_t count;
static int dc_level, resets, powers, bus_locks;
static bool stuck_busy, held_cs;
static int fail_after = -1;
static esp_err_t bus_error;
static int64_t time_us;
static uint8_t frame[48000], shadow[48000], line[100];

esp_err_t gpio_config(const gpio_config_t *config) { assert(config); return ESP_OK; }
esp_err_t gpio_reset_pin(gpio_num_t pin) { (void)pin; return ESP_OK; }
esp_err_t spi_bus_remove_device(spi_device_handle_t device) { assert(device); return ESP_OK; }
esp_err_t solar_os_bus_spi_add_device(const char *name,
                                     const spi_device_interface_config_t *config,
                                     spi_device_handle_t *device)
{
    assert(strcmp(name, "spi0") == 0 && config->spics_io_num == 10);
    if (bus_error) return bus_error;
    *device = (void *)1; return ESP_OK;
}

void vTaskDelay(TickType_t ticks)
{
    assert(ticks > 0);
    time_us += ticks * SOLAR_OS_EPD_TEST_TICK_MS * 1000LL;
}
int64_t esp_timer_get_time(void) { return time_us; }
int gpio_get_level(gpio_num_t pin) { (void)pin; return stuck_busy ? 0 : 1; }
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    if (pin == 9) dc_level = level;
    if (pin == 46 && !level) resets++;
    if (pin == 7 && level) powers++;
    return ESP_OK;
}
esp_err_t spi_device_acquire_bus(spi_device_handle_t device, unsigned wait)
{
    assert(device && wait == portMAX_DELAY && !bus_locks);
    bus_locks++;
    return ESP_OK;
}
void spi_device_release_bus(spi_device_handle_t device)
{
    assert(device && bus_locks == 1 && !held_cs);
    bus_locks--;
}
esp_err_t spi_device_polling_transmit(spi_device_handle_t device, spi_transaction_t *t)
{
    assert(device);
    if (fail_after == 0) { fail_after = -1; return ESP_FAIL; }
    if (fail_after > 0) fail_after--;
    if (!t->length) { held_cs = false; return ESP_OK; }
    const uint8_t *data = t->flags & SPI_TRANS_USE_TXDATA ? t->tx_data : t->tx_buffer;
    const size_t length = t->length / 8;
    if (!dc_level) {
        assert(length == 1 && count < 80 && !held_cs);
        captures[count++] = (capture_t){.command = data[0]};
    } else {
        assert(count);
        capture_t *c = &captures[count - 1];
        assert(c->length + length <= sizeof(c->data));
        memcpy(c->data + c->length, data, length);
        c->length += length;
    }
    held_cs = (t->flags & SPI_TRANS_CS_KEEP_ACTIVE) != 0;
    if (held_cs) assert(bus_locks == 1);
    return ESP_OK;
}

static epd_ultrachip_t make_display(void)
{
    memset(frame, 0xff, sizeof(frame)); /* SolarOS 1 = white. */
    memset(shadow, 0xff, sizeof(shadow));
    count = 0;
    return (epd_ultrachip_t){.spi = (void *)1, .buffer = frame, .shadow = shadow,
        .line_buffer = line, .panel = &panel_pro, .dc_pin = 9, .reset_pin = 46,
        .busy_pin = 3, .power_pin = 7};
}
static capture_t *find(uint8_t cmd, unsigned occurrence)
{
    for (unsigned i = 0; i < count; ++i)
        if (captures[i].command == cmd && occurrence-- == 0) return &captures[i];
    assert(!"missing command"); return NULL;
}
static bool has(uint8_t cmd)
{
    for (unsigned i = 0; i < count; ++i) if (captures[i].command == cmd) return true;
    return false;
}
static void expect(uint8_t cmd, const uint8_t *data, size_t len)
{
    capture_t *c = find(cmd, 0);
    assert(c->length == len && memcmp(c->data, data, len) == 0);
}

static void test_full_and_partial(void)
{
    epd_ultrachip_t d = make_display();
    frame[0] &= (uint8_t)~1U; frame[799] &= (uint8_t)~1U;
    frame[47999] &= (uint8_t)~0x80U;
    assert(epd_ultrachip_refresh(&d) == ESP_OK);
    assert(resets == 1 && powers == 1 && d.shadow_valid && !d.analog_on);
    expect(0x61, (uint8_t[]){3, 0x20, 2, 0x58}, 4);
    expect(0x65, (uint8_t[]){0,0,0,0}, 4);
    expect(0x30, (uint8_t[]){0x0e}, 1);
    expect(0x50, (uint8_t[]){0x97}, 1);
    assert(!has(0x06) && !has(0x01) && !has(0x82));
    assert(find(0x04, 0) < find(0x00, 1) && find(0x00, 1) < find(0x12, 0));
    assert(find(0x00, 1)->data[0] == 0x17 && find(0x00, 1)->data[1] == 0x4d);
    expect(0xe5, (uint8_t[]){0x1e}, 1);
    assert(!has(0x91));
    capture_t *new = find(0x13, 0), *old = find(0x10, 0);
    assert(new->length == 60000 && old->length == 60000);
    assert(new->data[12000] == 0x7f && new->data[12099] == 0xfe);
    assert(new->data[59999] == 0xfe); /* forward row order */
    for (unsigned i = 12000; i < 60000; ++i) assert((new->data[i] ^ old->data[i]) == 0xff);
    for (unsigned i = 0; i < 12000; ++i) assert(new->data[i] == 0xff && old->data[i] == 0xff);
    assert(memcmp(find(0x10, 1)->data, new->data, 60000) == 0);
    assert(memcmp(frame, shadow, 48000) == 0);
    count = 0;
    assert(epd_ultrachip_refresh(&d) == ESP_OK && count == 0);

    frame[0] = 0xff; /* erasing old ink must use the previous black pixel */
    assert(epd_ultrachip_refresh(&d) == ESP_OK);
    expect(0xe5, (uint8_t[]){0x5a}, 1);
    assert(has(0x91) && has(0x92));
    assert(find(0x10, 0)->data[12000] == 0x7f);
    assert(find(0x13, 0)->data[47900] == 0xff);
    expect(0x50, (uint8_t[]){0xd7}, 1);
    expect(0x90, (uint8_t[]){0,0,3,0x1f,0,0x78,2,0x57,1}, 9);
    assert(find(0x04, 0) < find(0x91, 0));
    assert(find(0x91, 0) < find(0x90, 0) && find(0x90, 0) < find(0x00, 0));
    assert(find(0x00, 0) < find(0x12, 0));
    assert(d.partial_count == 1 && resets == 1);
    count = 0; d.partial_count = 19; frame[0] &= (uint8_t)~1U;
    assert(epd_ultrachip_refresh(&d) == ESP_OK && !has(0x91));
    assert(d.partial_count == 0);
    count = 0;
    assert(epd_ultrachip_set_controller_mode(&d, "refresh=full") == ESP_OK);
    assert(epd_ultrachip_refresh(&d) == ESP_OK && has(0x12));
    assert(epd_ultrachip_set_controller_mode(&d, "invalid") == ESP_ERR_INVALID_ARG);
}

static void test_failures_and_sleep(void)
{
    epd_ultrachip_t d = make_display(); d.controller_ready = true; d.shadow_valid = true;
    frame[0] &= (uint8_t)~1U; stuck_busy = true;
    assert(epd_ultrachip_refresh(&d) == ESP_ERR_TIMEOUT);
    assert(!d.controller_ready && !d.shadow_valid && shadow[0] == 0xff);
    stuck_busy = false;
    count = 0;
    assert(epd_ultrachip_refresh(&d) == ESP_OK && !has(0x91));
    count = 0; frame[0] = 0xff; fail_after = 5;
    assert(epd_ultrachip_refresh(&d) == ESP_FAIL);
    assert(!d.shadow_valid && !d.controller_ready && !bus_locks && !held_cs);
    count = 0;
    assert(epd_ultrachip_refresh(&d) == ESP_OK);
    count = 0;
    assert(sleep_panel(&d) == ESP_OK);
    expect(0x07, (uint8_t[]){0xa5}, 1);
    assert(!d.controller_ready && !d.shadow_valid && !d.powered);
    count = 0;
    assert(epd_ultrachip_refresh(&d) == ESP_OK && has(0x61) && !has(0x91));
    d.shadow = NULL; count = 0;
    assert(epd_ultrachip_refresh(&d) == ESP_OK && !has(0x91));
    epd_ultrachip_config_t c = {.panel = 0};
    assert(epd_uc8279_init(&d, &c) == ESP_ERR_INVALID_ARG);
    assert(!config_valid(&c));
}

static void test_classic_profile(void)
{
    epd_ultrachip_t d = make_display();
    d.panel = &panel_classic;
    assert(epd_ultrachip_refresh(&d) == ESP_OK);
    assert(!has(0x30)); /* Classic retains its programmed PLL. */
    assert(find(0x04, 0) < find(0x00, 1));
    epd_ultrachip_config_t c = {.panel = 3};
    assert(epd_uc8279_init(&d, &c) == ESP_ERR_INVALID_ARG);
}

static void test_lifecycle(void)
{
    epd_ultrachip_t d = {0};
    epd_ultrachip_config_t c = {.spi_bus = "spi0", .cs_pin = 10, .dc_pin = 9,
        .reset_pin = 46, .busy_pin = 3, .power_pin = 7, .spi_clock_hz = 16000000,
        .rotation = U8G2_R1, .panel = EPD_UC8279_PANEL_X4_PRO_800X480};
    count = 0; bus_error = ESP_ERR_TIMEOUT;
    assert(epd_uc8279_init(&d, &c) == ESP_ERR_TIMEOUT && !d.spi && !d.buffer);
    bus_error = ESP_OK;
    assert(epd_uc8279_init(&d, &c) == ESP_OK && d.controller_ready && !d.shadow_valid);
    assert(d.u8g2.width == 480 && d.u8g2.height == 800 && d.u8g2.tile_buf_ptr == d.buffer);
    memset(d.buffer, 0xff, 48000); count = 0;
    u8g2_SendBuffer(&d.u8g2);
    assert(d.last_error == ESP_OK && d.shadow_valid);
    count = 0; u8g2_SetPowerSave(&d.u8g2, 1);
    assert(!d.powered && !d.shadow_valid);
    count = 0;
    assert(epd_ultrachip_resume(&d) == ESP_OK && d.controller_ready && !d.shadow_valid);
    count = 0; epd_ultrachip_deinit(&d);
    assert(!d.spi && !d.buffer && !d.line_buffer && !d.pins_configured);
    epd_ultrachip_deinit(&d); /* idempotent cleanup */
}

#define RELAX_TYPE epd_ultrachip_t
static bool relax_classic;
#define RELAX_SETUP(d) ((d).panel = relax_classic ? &panel_classic : &panel_pro)
#define RELAX_REFRESH epd_ultrachip_refresh
#define RELAX_APPLY epd_ultrachip_relax
#define RELAX_MODE epd_ultrachip_set_controller_mode
#define RELAX_PARTIAL EPD_ULTRACHIP_REFRESH_PARTIAL
#define RELAX_COUNT count
#define RELAX_WIRE find(0x13, 0)->data + d.panel->gate_offset * EPD_ULTRACHIP_ROW_BYTES
#include "epd_relax_contract.h"

int main(void)
{
    test_full_and_partial(); test_failures_and_sleep(); test_classic_profile(); test_lifecycle();
    test_relax_transaction();
    relax_classic = true;
    test_relax_transaction();
    puts("epd_uc8279_test: PASS"); return 0;
}

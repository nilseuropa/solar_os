#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src/drivers/epd_ssd1680.c"
#include "ssd1680_memory_stubs.h"

typedef struct { uint8_t command; size_t length; uint8_t data[4000]; } capture_t;
static capture_t captures[128];
static size_t count;
static int dc_level, bus_locks, resets, removed;
static int64_t time_us, busy_until;
static bool stuck_busy, held_cs;
static int fail_after = -1;
static esp_err_t bus_error;

esp_err_t gpio_config(const gpio_config_t *config) { assert(config); return ESP_OK; }
esp_err_t gpio_reset_pin(gpio_num_t pin) { (void)pin; return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    if (pin == 9) dc_level = level;
    if (pin == 46 && !level) resets++;
    return ESP_OK;
}
int gpio_get_level(gpio_num_t pin) { assert(pin == 3); return stuck_busy || time_us < busy_until; }
void vTaskDelay(TickType_t ticks)
{
    assert(ticks > 0 && !bus_locks);
    time_us += ticks * SOLAR_OS_EPD_TEST_TICK_MS * 1000LL;
}
int64_t esp_timer_get_time(void) { return time_us; }
esp_err_t solar_os_bus_spi_add_device(const char *name, const spi_device_interface_config_t *config,
                                      spi_device_handle_t *device)
{
    assert(!strcmp(name, "spi0") && config->spics_io_num == 10);
    assert(config->clock_speed_hz == 4000000);
    if (bus_error) return bus_error;
    *device = (void *)1;
    return ESP_OK;
}
esp_err_t spi_bus_remove_device(spi_device_handle_t device)
{
    assert(device && !bus_locks); removed++; return ESP_OK;
}
esp_err_t spi_device_acquire_bus(spi_device_handle_t device, unsigned wait)
{
    assert(device && wait == portMAX_DELAY && !bus_locks); bus_locks++; return ESP_OK;
}
void spi_device_release_bus(spi_device_handle_t device)
{
    assert(device && bus_locks == 1 && !held_cs); bus_locks--;
}
esp_err_t spi_device_polling_transmit(spi_device_handle_t device, spi_transaction_t *t)
{
    assert(device);
    if (fail_after == 0) { fail_after = -1; return ESP_FAIL; }
    if (fail_after > 0) fail_after--;
    if (!t->length) { held_cs = false; return ESP_OK; }
    const uint8_t *data = t->flags & SPI_TRANS_USE_TXDATA ? t->tx_data : t->tx_buffer;
    size_t length = t->length / 8U;
    if (!dc_level) {
        assert(length == 1 && count < 128 && !held_cs);
        captures[count++] = (capture_t){.command = data[0]};
        if (data[0] == 0x20) busy_until = time_us + 25000;
    } else {
        assert(count);
        capture_t *capture = &captures[count - 1U];
        assert(capture->length + length <= sizeof(capture->data));
        memcpy(capture->data + capture->length, data, length);
        capture->length += length;
    }
    held_cs = (t->flags & SPI_TRANS_CS_KEEP_ACTIVE) != 0;
    if (held_cs) assert(bus_locks == 1);
    return ESP_OK;
}

static capture_t *find(uint8_t cmd, unsigned occurrence)
{
    for (size_t i = 0; i < count; i++)
        if (captures[i].command == cmd && occurrence-- == 0) return &captures[i];
    assert(!"missing command"); return NULL;
}

static epd_ssd1680_config_t config(void)
{
    return (epd_ssd1680_config_t){.spi_bus = "spi0", .cs_pin = 10, .dc_pin = 9,
        .reset_pin = 46, .busy_pin = 3, .power_pin = 7, .spi_clock_hz = 4000000,
        .rotation = U8G2_R0}; /* No explicit panel: 122x250 is the default. */
}

static epd_ssd1680_t create(void)
{
    /* Initialize in place at the caller: u8g2 and the lock retain addresses. */
    return (epd_ssd1680_t){0};
}

static void test_geometry_padding_and_refresh(void)
{
    epd_ssd1680_t d = create();
    epd_ssd1680_config_t c = config();
    assert(epd_ssd1680_init(&d, &c) == ESP_OK);
    assert(u8g2_GetDisplayWidth(&d.u8g2) == 122 && u8g2_GetDisplayHeight(&d.u8g2) == 250);
    assert(u8g2_GetBufferSize(&d.u8g2) == 4096);
    assert(find(0x01, 0)->length == 3 && find(0x01, 0)->data[0] == 249);
    assert(!memcmp(find(0x44, 0)->data, (uint8_t[]){0, 15}, 2));
    assert(!memcmp(find(0x45, 0)->data, (uint8_t[]){0, 0, 249, 0}, 4));
    u8g2_SetDrawColor(&d.u8g2, 0);
    u8g2_DrawPixel(&d.u8g2, 0, 0);
    u8g2_DrawPixel(&d.u8g2, 121, 249);
    count = 0;
    assert(epd_ssd1680_refresh(&d) == ESP_OK);
    assert(find(0x22, 0)->data[0] == 0xf7 && d.shadow_valid);
    capture_t *plane = find(0x24, 0);
    assert(plane->length == 4000 && plane->data[0] == 0x7f && plane->data[3999] == 0xbf);
    assert(!memcmp(find(0x26, 1)->data, plane->data, 4000));
    count = 0;
    assert(epd_ssd1680_refresh(&d) == ESP_OK && count == 0);
    /* Invisible buffer columns and bottom-row bits cannot trigger updates. */
    d.buffer[127] = 0;
    d.buffer[31U * 128U] &= 3;
    assert(epd_ssd1680_refresh(&d) == ESP_OK && count == 0);
    for (unsigned update = 1; update <= 6; update++) {
        count = 0; d.buffer[0] ^= 1U;
        assert(epd_ssd1680_refresh(&d) == ESP_OK);
        assert(find(0x22, 0)->data[0] == (update == 6 ? 0xf7 : 0xff));
        assert(d.partial_count == (update == 6 ? 0 : update));
        if (update == 1) {
            assert(find(0x26, 0)->data[0] == 0x7f);
            assert(find(0x24, 0)->data[0] == 0xff && find(0x26, 1)->data[0] == 0xff);
        }
    }
    count = 0;
    assert(epd_ssd1680_present_power_state(&d) == ESP_OK && find(0x22, 0)->data[0] == 0xf7);
    assert(epd_ssd1680_set_controller_mode(&d, "refresh=full") == ESP_OK);
    count = 0;
    assert(epd_ssd1680_refresh(&d) == ESP_OK && find(0x22, 0)->data[0] == 0xf7);
    assert(strstr(epd_ssd1680_controller_mode(&d), "refresh=full"));
    assert(epd_ssd1680_set_controller_mode(&d, "refresh=pulse") == ESP_ERR_NOT_SUPPORTED);
    epd_ssd1680_deinit(&d);
}

static void test_rotated_raster_and_lifecycle(void)
{
    const u8g2_cb_t *rotations[] = {U8G2_R0, U8G2_R1, U8G2_R2, U8G2_R3};
    for (unsigned rotation = 0; rotation < 4; rotation++) {
        count = 0;
        epd_ssd1680_t d = create();
        epd_ssd1680_config_t c = config(); c.rotation = rotations[rotation];
        assert(epd_ssd1680_init(&d, &c) == ESP_OK);
        const unsigned width = rotation % 2 ? 250 : 122, height = rotation % 2 ? 122 : 250;
        assert(u8g2_GetDisplayWidth(&d.u8g2) == width && u8g2_GetDisplayHeight(&d.u8g2) == height);
        const uint8_t bitmap[] = {0xf5, 0xee, 0xea, 0xdd};
        uint8_t expected[EPD_SSD1680_BUFFER_BYTES];
        u8g2_t reference = d.u8g2;
        u8g2_SetupBuffer(&reference, expected, EPD_SSD1680_TILE_ROWS,
                         u8g2_ll_hvline_vertical_top_lsb, rotations[rotation]);
        for (unsigned inverted = 0; inverted < 2; inverted++) {
            memset(expected, inverted ? 0 : 0xff, sizeof(expected));
            for (unsigned y = 0; y < 2; y++)
                for (unsigned x = 0; x < 5; x++) {
                    bool black = (bitmap[y * 2] & (1U << x)) != 0;
                    u8g2_SetDrawColor(&reference, black == (bool)inverted);
                    u8g2_DrawPixel(&reference, width - 5 + x, height - 2 + y);
                }
            count = 0;
            assert(epd_ssd1680_present_mono_xbm(&d, bitmap, 4, width - 5, height - 2, 5, 2, 2, inverted) == ESP_OK);
            assert(!memcmp(d.buffer, expected, sizeof(expected)));
            const capture_t *plane = find(0x24, 0);
            for (unsigned y = 0; y < 250; y++) assert((plane->data[y * 16 + 15] & 0x3f) == 0x3f);
        }
        count = 0;
        assert(epd_ssd1680_present_mono_xbm(&d, bitmap, 3, 0, 0, 5, 2, 2, false) == ESP_ERR_INVALID_ARG);
        assert(epd_ssd1680_present_mono_xbm(&d, bitmap, 4, width - 4, 0, 5, 2, 2, false) == ESP_ERR_INVALID_ARG);
        assert(!count && !memcmp(d.buffer, expected, sizeof(expected)));
        u8g2_SetPowerSave(&d.u8g2, 1);
        assert(d.suspended && !d.powered && !d.controller_ready && !d.shadow_valid);
        count = 0;
        assert(epd_ssd1680_present_mono_xbm(&d, bitmap, 4, 0, 0, 5, 2, 2, false) == ESP_ERR_INVALID_STATE);
        assert(!count && !memcmp(d.buffer, expected, sizeof(expected)));
        assert(epd_ssd1680_resume(&d) == ESP_OK);
        assert(!d.suspended && d.controller_ready && !d.shadow_valid);
        epd_ssd1680_deinit(&d);
        assert(!d.spi && !d.buffer && !d.shadow && !d.line_buffer);
    }
}

static void test_failure_recovery_and_relax(void)
{
    count = 0;
    epd_ssd1680_t d = create();
    epd_ssd1680_config_t c = config();
    assert(epd_ssd1680_init(&d, &c) == ESP_OK);
    assert(epd_ssd1680_refresh(&d) == ESP_OK);
    uint8_t original[EPD_SSD1680_BUFFER_BYTES];
    memcpy(original, d.buffer, sizeof(original));
    d.buffer[0] ^= 1U;
    count = 0; fail_after = 20; /* Fail within a RAM plane with CS held. */
    assert(epd_ssd1680_refresh(&d) == ESP_FAIL);
    assert(!d.shadow_valid && !d.controller_ready && !bus_locks && !held_cs);
    count = 0;
    assert(epd_ssd1680_refresh(&d) == ESP_OK && find(0x22, 0)->data[0] == 0xf7);
    d.buffer[0] ^= 1U; count = 0; stuck_busy = true;
    assert(epd_ssd1680_refresh(&d) == ESP_ERR_TIMEOUT);
    assert(!d.shadow_valid && !d.controller_ready && !bus_locks);
    stuck_busy = false; count = 0;
    assert(epd_ssd1680_refresh(&d) == ESP_OK && find(0x22, 0)->data[0] == 0xf7);
    assert(epd_ssd1680_relax(&d, SOLAR_OS_DISPLAY_RELAX_BEGIN) == ESP_OK);
    assert(epd_ssd1680_set_controller_mode(&d, "refresh=full") == ESP_ERR_INVALID_STATE);
    count = 0;
    assert(epd_ssd1680_refresh(&d) == ESP_OK && !count);
    assert(epd_ssd1680_relax(&d, SOLAR_OS_DISPLAY_RELAX_BLACK) == ESP_OK);
    const capture_t *black = find(0x24, 0);
    for (unsigned i = 0; i < 4000; i++) assert(black->data[i] == (i % 16 == 15 ? 0x3f : 0));
    count = 0;
    assert(epd_ssd1680_relax(&d, SOLAR_OS_DISPLAY_RELAX_END) == ESP_OK);
    assert(!d.relax.storage && d.shadow_valid && !memcmp(d.buffer, original, sizeof(original)));
    epd_ssd1680_deinit(&d);
    count = 0; bus_error = ESP_FAIL;
    assert(epd_ssd1680_init(&d, &c) == ESP_FAIL);
    assert(!d.spi && !d.buffer && !d.line_buffer);
    bus_error = ESP_OK;
    for (int allocation = 0; allocation < 3; allocation++) {
        count = 0; memory_fail_after = allocation;
        assert(epd_ssd1680_init(&d, &c) == ESP_ERR_NO_MEM);
        assert(!d.spi && !d.buffer && !d.shadow && !d.line_buffer && !memory_allocations);
    }
    memory_fail_after = -1;
    c.panel = 1; assert(epd_ssd1680_init(&d, &c) == ESP_ERR_INVALID_ARG);
    c.panel = 0; c.reset_pin = c.cs_pin;
    assert(epd_ssd1680_init(&d, &c) == ESP_ERR_INVALID_ARG);
    c = config(); c.spi_clock_hz = 4000001;
    assert(epd_ssd1680_init(&d, &c) == ESP_ERR_INVALID_ARG);
}

int main(void)
{
    test_geometry_padding_and_refresh();
    test_rotated_raster_and_lifecycle();
    test_failure_recovery_and_relax();
    assert(!bus_locks && !held_cs && removed == 9 && resets > 0 && !memory_allocations);
    puts("SSD1680: 122x250 geometry, padded RAM, rotation, lifecycle and recovery passed");
    return 0;
}

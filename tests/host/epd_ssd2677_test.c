#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src/drivers/epd_ssd2677.c"
#include "ssd2677_memory_stubs.h"

typedef struct { uint8_t cmd; size_t size; uint8_t data[SSD2677_BUFFER_BYTES * 2U]; } captured_t;
static captured_t captured[64];
static size_t command_count;
static int dc_level, fail_after = -1;
static int64_t now_us, busy_until;
static bool stuck_busy;
static uint8_t sensor_temperature = 25;
static unsigned removed, freed_bus;
static unsigned reset_pins;
static bool private_bus;

esp_err_t gpio_config(const gpio_config_t *gpio) { assert(gpio); return ESP_OK; }
esp_err_t gpio_reset_pin(gpio_num_t pin) { (void)pin; reset_pins++; return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{ if (pin == 9) dc_level = level; return ESP_OK; }
int gpio_get_level(gpio_num_t pin) { (void)pin; return !(stuck_busy || now_us < busy_until); }
int64_t esp_timer_get_time(void) { return now_us; }
void vTaskDelay(TickType_t ticks)
{ assert(ticks > 0); now_us += (int64_t)ticks * SOLAR_OS_EPD_TEST_TICK_MS * 1000; }
esp_err_t spi_bus_initialize(spi_host_device_t host, const spi_bus_config_t *bus, int dma)
{ (void)host; (void)bus; (void)dma; return ESP_OK; }
esp_err_t spi_bus_add_device(spi_host_device_t host, const spi_device_interface_config_t *config, spi_device_handle_t *spi)
{
    (void)host;
    assert((config->flags & (SPI_DEVICE_3WIRE | SPI_DEVICE_HALFDUPLEX)) ==
        (SPI_DEVICE_3WIRE | SPI_DEVICE_HALFDUPLEX));
    *spi = (void *)1; return ESP_OK;
}
esp_err_t solar_os_bus_spi_add_device(const char *bus, const spi_device_interface_config_t *config, spi_device_handle_t *spi)
{ assert(!strcmp(bus,"spi0")); return spi_bus_add_device(SPI2_HOST,config,spi); }
esp_err_t spi_bus_remove_device(spi_device_handle_t spi) { assert(spi); removed++; return ESP_OK; }
esp_err_t spi_bus_free(spi_host_device_t host) { (void)host; freed_bus++; return ESP_OK; }
esp_err_t spi_device_polling_transmit(spi_device_handle_t spi, spi_transaction_t *tx)
{
    assert(spi);
    if (fail_after == 0) { fail_after = -1; return ESP_FAIL; }
    if (fail_after > 0) fail_after--;
    if (tx->rxlength) {
        assert(tx->rxlength == 8 && tx->override_freq_hz == 1000000);
        *(uint8_t *)tx->rx_buffer = sensor_temperature;
        return ESP_OK;
    }
    const uint8_t *data = tx->flags & SPI_TRANS_USE_TXDATA ? tx->tx_data : tx->tx_buffer;
    const size_t size = tx->length / 8U;
    if (!dc_level) {
        assert(size == 1 && command_count < 64);
        captured[command_count++] = (captured_t){.cmd = data[0]};
        if (data[0] == 0x12 || data[0] == 0x04 || data[0] == 0x02 || data[0] == 0xa5)
            busy_until = now_us + 30000;
    } else {
        assert(command_count);
        captured_t *c = &captured[command_count - 1];
        assert(c->size + size <= sizeof(c->data));
        memcpy(c->data + c->size, data, size); c->size += size;
    }
    return ESP_OK;
}

static const captured_t *find(uint8_t command)
{
    for (size_t i = 0; i < command_count; i++) if (captured[i].cmd == command) return &captured[i];
    return NULL;
}

static epd_ssd2677_t make_display(void)
{
    epd_ssd2677_t d;
    const epd_ssd2677_config_t config = {
        .spi_bus = private_bus ? NULL : "spi0", .spi_host = SPI2_HOST, .sclk_pin = 13, .mosi_pin = 14,
        .cs_pin = 15, .dc_pin = 9, .reset_pin = 17, .busy_pin = 18, .power_pin = 47,
        .busy_level = 0, .power_active_level = 1, .panel = 1,
        .spi_clock_hz = 20000000, .rotation = U8G2_R0,
    };
    command_count = 0;
    assert(epd_ssd2677_init(&d,&config) == ESP_OK);
    /* A returned static semaphore pointer must refer to this instance. */
    d.lock = xSemaphoreCreateMutexStatic(&d.lock_storage);
    assert(find(0x61)->size == 4 && !memcmp(find(0x61)->data, (uint8_t[]){3,32,2,168},4));
    assert(find(0x00)->data[0] == 0x2f && !find(0x12));
    memset(d.buffer,0xff,d.buffer_size);
    command_count = 0;
    return d;
}

static void check_wire(const epd_ssd2677_t *d, const uint8_t *old)
{
    const captured_t *wire = find(0x10);
    assert(wire && wire->size == 96000);
    for (unsigned y = 0; y < 480; y++) for (unsigned x = 0; x < 800; x++) {
        /* Independent wire-to-logical mapping for the reversed V2 gate scan. */
        const unsigned logical = (479U - y) / 8U * 800U + x;
        const unsigned shift = (479U - y) & 7U;
        unsigned n = (d->buffer[logical] >> shift) & 1U;
        unsigned p = old ? (old[logical] >> shift) & 1U : n;
        assert(((wire->data[y * 200U + x / 4U] >> (6U - (x & 3U) * 2U)) & 3U) == (p * 2U + n));
    }
}

static void test_refresh_and_cleanup(void)
{
    epd_ssd2677_t d = make_display();
    d.buffer[0] &= 0xfe; d.buffer[47999] &= 0x7f;
    d.buffer[10] &= 0xfd; /* asymmetric marker: logical x=10, y=1 */
    assert(refresh(&d) == ESP_OK && d.shadow_valid && !d.analog_on);
    assert(find(0xe6)->data[0] == 0xee && find(0x04) && find(0x12) && find(0x02));
    check_wire(&d,NULL);
    assert((find(0x10)->data[95800] & 0xc0U) == 0); /* logical top-left */
    assert((find(0x10)->data[199] & 3U) == 0); /* logical bottom-right */
    assert((find(0x10)->data[95602] & 0x0cU) == 0); /* physical x=10, y=478 */
    command_count = 0;
    assert(refresh(&d) == ESP_OK && !command_count);
    uint8_t old[48000]; memcpy(old,d.shadow,sizeof(old));
    d.buffer[0] |= 1; d.buffer[810] &= 0xfb;
    assert(refresh(&d) == ESP_OK && d.analog_on && d.partial_refresh_count == 1);
    assert(find(0xe6)->data[0] == 0x19 && !find(0x02));
    check_wire(&d,old);
    for (unsigned n = 1; n <= 19; n++) {
        command_count = 0; d.buffer[1] ^= 1;
        assert(refresh(&d) == ESP_OK);
        assert(find(0xe6)->data[0] == (n == 19 ? 0xee : 0x19));
    }
    assert(d.partial_refresh_count == 0 && !d.analog_on);
    epd_ssd2677_deinit(&d);
    assert(removed == 1 && freed_bus == 0);
}

static void test_temperature(void)
{
    const uint8_t temperatures[] = {0,1,10,11,20,21,30,31,127,128,255};
    const uint8_t full[] = {0xee,0xe8,0xe8,0xeb,0xeb,0xee,0xee,0xf1,0xf1,0xee,0xee};
    const uint8_t partial[] = {0x19,0x08,0x08,0x0f,0x0f,0x19,0x19,0x23,0x23,0x19,0x19};
    epd_ssd2677_t d = make_display();
    for (unsigned t = 0; t < sizeof(temperatures); t++) for (unsigned p = 0; p < 2; p++) {
        sensor_temperature = temperatures[t]; command_count = 0;
        assert(latch_waveform(&d,p) == ESP_OK);
        assert(find(0xe6)->data[0] == (p ? partial[t] : full[t]));
    }
    sensor_temperature = 25;
    command_count = 0; epd_ssd2677_deinit(&d);
}

static void test_failure_power_modes(void)
{
    epd_ssd2677_t d = make_display();
    assert(refresh(&d) == ESP_OK);
    command_count = 0; d.buffer[0] ^= 1; fail_after = 5;
    assert(refresh(&d) == ESP_FAIL && !d.shadow_valid && !d.controller_ready);
    assert(d.shadow[0] != d.buffer[0]);
    command_count = 0;
    assert(refresh(&d) == ESP_OK && find(0xe6)->data[0] == 0xee);
    command_count = 0; stuck_busy = true; d.buffer[1] ^= 1;
    assert(refresh(&d) == ESP_ERR_TIMEOUT && !d.shadow_valid);
    stuck_busy = false; command_count = 0;
    assert(refresh(&d) == ESP_OK && d.shadow_valid);
    assert(epd_ssd2677_set_controller_mode(&d,"refresh=fast") == ESP_OK);
    assert(d.refresh_mode == EPD_SSD2677_REFRESH_PARTIAL);
    assert(epd_ssd2677_set_controller_mode(&d,"refresh=pulse") == ESP_ERR_NOT_SUPPORTED);
    assert(epd_ssd2677_set_controller_mode(&d,"waveform=fastwave") == ESP_ERR_NOT_SUPPORTED);
    assert(d.refresh_mode == EPD_SSD2677_REFRESH_PARTIAL);
    command_count = 0;
    assert(epd_ssd2677_present_power_state(&d) == ESP_OK && find(0x02));
    command_count = 0;
    assert(refresh(&d) == ESP_ERR_INVALID_STATE && !command_count);
    assert(epd_ssd2677_resume(&d) == ESP_OK);
    command_count = 0;
    u8g2_SetPowerSave(&d.u8g2,1);
    assert(d.suspended && !d.powered && find(0x07)->data[0] == 0xa5);
    command_count = 0;
    assert(refresh(&d) == ESP_ERR_INVALID_STATE && !command_count);
    assert(epd_ssd2677_resume(&d) == ESP_OK && !d.suspended && !d.shadow_valid);
    command_count = 0;
    assert(refresh(&d) == ESP_OK && find(0xe6)->data[0] == 0xee);
    command_count = 0; epd_ssd2677_deinit(&d);
}

static void test_relax_and_shadow_failure(void)
{
    epd_ssd2677_t d = make_display();
    d.buffer[10] = 0x35;
    assert(refresh(&d) == ESP_OK);
    command_count = 0;
    assert(epd_ssd2677_relax(&d,SOLAR_OS_DISPLAY_RELAX_BEGIN) == ESP_OK);
    assert(epd_ssd2677_set_controller_mode(&d,"refresh=full") == ESP_ERR_INVALID_STATE);
    assert(refresh(&d) == ESP_OK && !command_count);
    assert(epd_ssd2677_relax(&d,SOLAR_OS_DISPLAY_RELAX_BLACK) == ESP_OK);
    assert(d.buffer[10] == 0x35 && find(0x10)->data[0] == 0);
    command_count = 0; fail_after = 0;
    assert(epd_ssd2677_relax(&d,SOLAR_OS_DISPLAY_RELAX_WHITE) == ESP_FAIL);
    assert(d.relax.storage && !d.shadow_valid);
    command_count = 0;
    assert(epd_ssd2677_relax(&d,SOLAR_OS_DISPLAY_RELAX_END) == ESP_OK);
    assert(d.shadow[10] == 0x35 && !d.relax.storage);
    command_count = 0;
    assert(epd_ssd2677_relax(&d,SOLAR_OS_DISPLAY_RELAX_BEGIN) == ESP_OK);
    d.powered = false;
    assert(epd_ssd2677_relax(&d,SOLAR_OS_DISPLAY_RELAX_END) == ESP_ERR_INVALID_STATE);
    assert(!d.relax.storage);
    d.powered = true;
    free(d.shadow); d.shadow = NULL;
    command_count = 0;
    assert(refresh(&d) == ESP_OK && find(0xe6)->data[0] == 0xee && !d.shadow_valid);
    command_count = 0; epd_ssd2677_deinit(&d);
}

static void test_raster_rotations(void)
{
    const u8g2_cb_t *rotations[] = {U8G2_R0,U8G2_R1,U8G2_R2,U8G2_R3};
    for (unsigned r = 0; r < 4; r++) {
        epd_ssd2677_t d = make_display();
        uint8_t expected[48000], bitmap[32];
        for (unsigned i = 0; i < sizeof(bitmap); i++) bitmap[i] = i * 73U;
        u8g2_SetDisplayRotation(&d.u8g2,rotations[r]);
        u8g2_t reference = d.u8g2;
        u8g2_SetupBuffer(&reference,expected,60,u8g2_ll_hvline_vertical_top_lsb,rotations[r]);
        for (unsigned aligned = 0; aligned < 2; aligned++) for (unsigned invert = 0; invert < 2; invert++) {
            unsigned x = aligned ? 8 : 3, y = aligned ? 8 : 7;
            memset(expected,invert ? 0 : 0xff,sizeof(expected));
            for (unsigned row = 0; row < 16; row++) for (unsigned column = 0; column < 16; column++) {
                bool black = (bitmap[row * 2 + column / 8] & (1U << (column % 8))) != 0;
                u8g2_SetDrawColor(&reference,black == (bool)invert);
                u8g2_DrawPixel(&reference,x + column,y + row);
            }
            command_count = 0;
            assert(epd_ssd2677_present_mono_xbm(&d,bitmap,sizeof(bitmap),x,y,16,16,2,invert) == ESP_OK);
            assert(!memcmp(d.buffer,expected,sizeof(expected)) && !memcmp(d.shadow,expected,sizeof(expected)));
        }
        command_count = 0;
        assert(epd_ssd2677_present_mono_xbm(&d,bitmap,31,0,0,16,16,2,false) == ESP_ERR_INVALID_ARG);
        assert(epd_ssd2677_present_mono_xbm(&d,bitmap,32,0,0,16,16,1,false) == ESP_ERR_INVALID_ARG);
        assert(epd_ssd2677_present_mono_xbm(&d,bitmap,32,UINT16_MAX,0,16,16,2,false) == ESP_ERR_INVALID_ARG);
        assert(!command_count);
        epd_ssd2677_deinit(&d);
    }
}

int main(void)
{
    epd_ssd2677_t rejected = {0};
    const epd_ssd2677_config_t invalid = {0};
    assert(epd_ssd2677_init(&rejected,&invalid) == ESP_ERR_INVALID_ARG);
    epd_ssd2677_deinit(&rejected);
    assert(reset_pins == 0); /* no peripheral GPIO reset on rejected bindings */
    test_refresh_and_cleanup(); test_temperature(); test_failure_power_modes();
    test_relax_and_shadow_failure(); test_raster_rotations();
    private_bus = true;
    epd_ssd2677_t d = make_display(); epd_ssd2677_deinit(&d);
    const unsigned released_pins = reset_pins;
    epd_ssd2677_deinit(&d);
    assert(reset_pins == released_pins); /* repeat cleanup is harmless */
    assert(freed_bus == 1);
    puts("epd_ssd2677_test: PASS");
    return 0;
}

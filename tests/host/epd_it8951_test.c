#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src/drivers/epd_it8951.c"
#include "ssd2677_memory_stubs.h"

static int64_t now;
static bool held, selected, stuck_ready, stuck_lut, wrong_panel;
static unsigned phase, preamble, current_command, arg_count, last_reg;
static unsigned updates, last_waveform, rows, removed;
static int fail_after = -1;
static int fail_power_pin = -1;
static uint16_t registers[0x1300];
static uint16_t args[8];
static uint8_t uploaded[(size_t)WIDTH * HEIGHT];
static unsigned levels[49];

int64_t esp_timer_get_time(void) { return now; }
void vTaskDelay(TickType_t ticks) { assert(ticks); now += (int64_t)ticks * 1000; }
esp_err_t gpio_config(const gpio_config_t *cfg) { assert(cfg); return ESP_OK; }
int gpio_get_level(gpio_num_t pin) { assert(pin == 13); return !stuck_ready; }
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    assert(pin >= 0 && pin < 49);
    if (pin == fail_power_pin) { fail_power_pin = -1; return ESP_FAIL; }
    levels[pin] = level;
    if (pin == 10) {
        if (!level) { assert(held && !selected); selected = true; phase = 0; }
        else selected = false;
    }
    return ESP_OK;
}
esp_err_t solar_os_bus_spi_add_device(const char *bus,
    const spi_device_interface_config_t *cfg, spi_device_handle_t *spi)
{
    assert(!strcmp(bus, "spi0") && cfg->spics_io_num == -1 && cfg->mode == 0);
    assert(cfg->clock_speed_hz == 10000000);
    *spi = (void *)1;
    return ESP_OK;
}
esp_err_t spi_bus_remove_device(spi_device_handle_t spi) { assert(spi); removed++; return ESP_OK; }
esp_err_t spi_device_acquire_bus(spi_device_handle_t spi, unsigned wait)
{ assert(spi && !held && wait == portMAX_DELAY); held = true; return ESP_OK; }
void spi_device_release_bus(spi_device_handle_t spi)
{ assert(spi && held && !selected); held = false; }

esp_err_t spi_device_polling_transmit(spi_device_handle_t spi, spi_transaction_t *t)
{
    assert(spi && selected && held);
    if (fail_after == 0) { fail_after = -1; return ESP_FAIL; }
    if (fail_after > 0) fail_after--;
    const uint8_t *tx = t->tx_buffer;
    const size_t size = t->length / 8U;
    if (!phase++) {
        assert(size == 2 && tx);
        preamble = ((unsigned)tx[0] << 8) | tx[1];
        assert(preamble == 0 || preamble == 0x1000 || preamble == 0x6000);
        return ESP_OK;
    }
    if (preamble == 0x1000) {
        if (phase == 2) { assert(size == 2 && !t->rx_buffer); return ESP_OK; }
        assert(phase == 3 && t->rx_buffer);
        uint8_t *rx = t->rx_buffer;
        memset(rx, 0, size);
        if (current_command == 0x0302) {
            assert(size == 40);
            const uint16_t info[] = {wrong_panel ? 800 : WIDTH, HEIGHT, 0x3456, 0x12};
            for (unsigned i = 0; i < 4; i++) { rx[2*i] = info[i] >> 8; rx[2*i+1] = info[i]; }
        } else {
            assert(current_command == 0x0010 && size == 2);
            const uint16_t value = last_reg == 0x1224 ? (unsigned)stuck_lut : registers[last_reg];
            rx[0] = value >> 8; rx[1] = value;
        }
    } else if (preamble == 0x6000) {
        assert(size == 2 && phase == 2);
        current_command = ((unsigned)tx[0] << 8) | tx[1]; arg_count = 0;
        if (current_command == 0x0021) rows = 0;
    } else if (size == WIDTH) {
        assert(current_command == 0x0021 && arg_count == 5 && rows < HEIGHT);
        memcpy(uploaded + (size_t)rows++ * WIDTH, tx, size);
    } else {
        assert(size == 2 && arg_count < 8);
        args[arg_count++] = (uint16_t)(((unsigned)tx[0] << 8) | tx[1]);
        if (current_command == 0x0010) last_reg = args[0];
        if (current_command == 0x0011 && arg_count == 2) registers[args[0]] = args[1];
        if (current_command == 0x0021 && arg_count == 5)
            assert(args[0] == 0x0130 && args[1] == 0 && args[2] == 0 && args[3] == WIDTH && args[4] == HEIGHT);
        if (current_command == 0x0039 && arg_count == 2) assert(args[0] == 2 && args[1] == 1400);
        if (current_command == 0x0034 && arg_count == 5) {
            assert(rows == HEIGHT); updates++; last_waveform = args[4];
        }
    }
    return ESP_OK;
}

static solar_os_it8951_config_t config(void)
{
    return (solar_os_it8951_config_t){.spi_bus="spi0", .cs=10, .reset=12, .busy=13,
        .power=11, .ite_power=21, .clock_hz=10000000, .rotation=0, .vcom_mv=1400};
}

int main(void)
{
    solar_os_it8951_t d;
    solar_os_it8951_config_t cfg = config();
    cfg.vcom_mv = 2330; assert(solar_os_it8951_init(&d, &cfg) == ESP_ERR_INVALID_ARG);
    cfg = config(); cfg.ite_power = cfg.cs;
    assert(solar_os_it8951_init(&d, &cfg) == ESP_ERR_INVALID_ARG);
    cfg = config(); wrong_panel = true;
    assert(solar_os_it8951_init(&d, &cfg) == ESP_ERR_INVALID_RESPONSE);
    assert(!d.spi && !levels[11] && !levels[21] && !held && !selected);
    wrong_panel = false;
    fail_power_pin = 21;
    assert(solar_os_it8951_init(&d, &cfg) == ESP_FAIL);
    assert(!d.spi && !levels[11] && !levels[21]);
    assert(solar_os_it8951_init(&d, &cfg) == ESP_OK);
    assert(d.ready && d.image_address == 0x123456 && !updates);
    assert(u8g2_GetDisplayWidth(&d.u8g2) == WIDTH && u8g2_GetDisplayHeight(&d.u8g2) == HEIGHT);
    /* Exercise real U8g2 addressing above 64 KiB, the final row and padded tiles. */
    u8g2_SetDrawColor(&d.u8g2, 0);
    u8g2_DrawPixel(&d.u8g2, 0, 0);
    u8g2_DrawPixel(&d.u8g2, WIDTH - 1, HEIGHT - 1);
    assert(d.mono[0] == 0xfe && d.mono[(size_t)((HEIGHT-1)/8)*WIDTH + WIDTH-1] == 0xf7);
    assert(solar_os_it8951_present(&d) == ESP_OK && updates == 1 && last_waveform == 2);
    assert(uploaded[WIDTH-1] == 0 && uploaded[0] == 255);
    assert(uploaded[(size_t)(HEIGHT-1)*WIDTH] == 0);
    assert(solar_os_it8951_present(&d) == ESP_OK && updates == 1);
    d.mono[1] = 1;
    assert(solar_os_it8951_present(&d) == ESP_OK && last_waveform == 1);
    assert(registers[0x208] == 0x3456 && registers[0x20a] == 0x12);
    for (unsigned i = 0; i < 19; i++) {
        d.mono[4] ^= 1;
        assert(solar_os_it8951_present(&d) == ESP_OK);
    }
    assert(last_waveform == 2); /* clean after twenty binary updates */
    const uint16_t palette[] = {0, 0x8410, 0xffff};
    uint8_t pixels[] = {0, 1, 2};
    solar_os_display_raster_t f = {.data=pixels, .data_size=3, .source_width=3, .source_height=1,
        .source_stride=3, .x=5, .y=7, .width=3, .height=1, .format=SOLAR_OS_DISPLAY_FORMAT_INDEX8,
        .palette_rgb565=palette, .palette_size=3};
    assert(solar_os_it8951_present_frame(&d, &f) == ESP_OK && last_waveform == 2);
    assert(gray_at(&d,5,7) == 0 && gray_at(&d,6,7) == 8 && gray_at(&d,7,7) == 15);
    assert(uploaded[(size_t)7*WIDTH+WIDTH-1-6] == 136);
    const uint8_t packed_pixels = 0x24; /* LSB-first palette indices 0,1,2 */
    solar_os_display_raster_t packed = f;
    packed.data = &packed_pixels; packed.data_size = 1; packed.source_stride = 1;
    packed.format = SOLAR_OS_DISPLAY_FORMAT_INDEX2;
    assert(solar_os_it8951_present_frame(&d,&packed) == ESP_OK);
    assert(gray_at(&d,5,7) == 0 && gray_at(&d,6,7) == 8 && gray_at(&d,7,7) == 15);
    const uint8_t rgb_pixels[] = {0xf8,0x00,0x00,0x1f}; /* wire-order red and blue */
    packed.data = rgb_pixels; packed.data_size = 4; packed.source_stride = 4;
    packed.format = SOLAR_OS_DISPLAY_FORMAT_RGB565; packed.source_width = packed.width = 2;
    assert(solar_os_it8951_present_frame(&d,&packed) == ESP_OK);
    assert(gray_at(&d,5,7) == 5 && gray_at(&d,6,7) == 2);
    const unsigned before = updates;
    pixels[1] = 3;
    assert(solar_os_it8951_present_frame(&d, &f) == ESP_ERR_INVALID_ARG && updates == before);
    pixels[1] = 1; f.data_size=2;
    assert(solar_os_it8951_present_frame(&d, &f) == ESP_ERR_INVALID_SIZE);
    f.data_size=3;
    assert(solar_os_it8951_present(&d) == ESP_OK && last_waveform == 2);
    /* DU must not be used to transition old gray pixels back to monochrome. */
    for (unsigned r = 0; r < 4; r++) {
        static const u8g2_cb_t *const cb[] = {U8G2_R0,U8G2_R1,U8G2_R2,U8G2_R3};
        u8g2_SetDisplayRotation(&d.u8g2, cb[r]);
        f.x=0; f.y=0; f.width=3; f.height=1;
        assert(solar_os_it8951_present_frame(&d, &f) == ESP_OK);
        static const unsigned xs[] = {1, WIDTH-1, WIDTH-2, 0};
        static const unsigned ys[] = {0, 1, HEIGHT-1, HEIGHT-2};
        assert(gray_at(&d,xs[r],ys[r]) == 8);
    }
    u8g2_SetDisplayRotation(&d.u8g2, U8G2_R0);
    const uint8_t white_pixel = 1;
    f = (solar_os_display_raster_t){.data=&white_pixel, .data_size=1,
        .source_width=1, .source_height=1, .source_stride=1, .width=1, .height=1,
        .format=SOLAR_OS_DISPLAY_FORMAT_MONO1, .palette_inverted=true,
        .clear_background=true, .background_index=0};
    assert(solar_os_it8951_present_frame(&d,&f) == ESP_OK);
    assert(gray_at(&d,0,0) == 15 && gray_at(&d,1,0) == 0);
    assert(solar_os_it8951_set_power_save(&d,true) == ESP_OK);
    assert(!d.ready && !levels[11] && !levels[21]);
    assert(solar_os_it8951_set_power_save(&d,false) == ESP_OK && d.ready && !d.shadow_valid);
    fail_after = 2;
    assert(solar_os_it8951_present(&d) == ESP_FAIL && !d.shadow_valid && !d.ready);
    assert(!held && !selected);
    assert(solar_os_it8951_present(&d) == ESP_OK && last_waveform == 2);
    stuck_ready = true;
    d.mono[2] ^= 1;
    assert(solar_os_it8951_present(&d) == ESP_ERR_TIMEOUT && !held && !selected);
    stuck_ready = false;
    assert(solar_os_it8951_present(&d) == ESP_OK);
    stuck_lut = true; d.mono[3] ^= 1;
    assert(solar_os_it8951_present(&d) == ESP_ERR_TIMEOUT && !d.shadow_valid);
    stuck_lut = false;
    assert(solar_os_it8951_present(&d) == ESP_OK);
    solar_os_it8951_deinit(&d);
    assert(!held && !selected && !d.spi && removed == 3);
    puts("IT8951 protocol, framebuffer, grayscale, rotation and recovery tests passed");
    return 0;
}

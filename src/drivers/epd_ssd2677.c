/* Sticky 3D97 V2 monochrome panel protocol:
 * Seeed-Studio/Seeed_GFX2 834c8cafa108805f2b625732998b86b31d683da1,
 * src/driver/epaper/Driver_SSD2677.cpp. Register values describe this panel,
 * not generic SSD2677 color glass. No SSD1677 LUT/registers are used here.
 * See doc/licenses/seeed-ssd2677-notice.txt.
 */
#include "epd_ssd2677.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "solar_os_memory.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "solar_os_buses.h"

#define SSD2677_WIDTH 800U
#define SSD2677_HEIGHT 480U
#define SSD2677_BUFFER_BYTES (SSD2677_WIDTH * SSD2677_HEIGHT / 8U)
#define SSD2677_ROW_BYTES (SSD2677_WIDTH / 4U)
#define TRY(call) do { esp_err_t err_ = (call); if (err_ != ESP_OK) return err_; } while (0)

static void delay_ms(unsigned ms)
{
    const TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

static esp_err_t command(epd_ssd2677_t *d, uint8_t cmd, const uint8_t *data, size_t size)
{
    TRY(gpio_set_level(d->dc_pin, 0));
    spi_transaction_t tx = {.flags = SPI_TRANS_USE_TXDATA, .length = 8, .tx_data = {cmd}};
    TRY(spi_device_polling_transmit(d->spi, &tx));
    if (size) {
        /* All command parameters fit the internal DMA staging buffer. */
        if (!data || size > d->line_buffer_size) return ESP_ERR_INVALID_ARG;
        memcpy(d->line_buffer, data, size);
        TRY(gpio_set_level(d->dc_pin, 1));
        tx = (spi_transaction_t){.length = size * 8U, .tx_buffer = d->line_buffer};
        TRY(spi_device_polling_transmit(d->spi, &tx));
    }
    return ESP_OK;
}

static esp_err_t wait_ready(epd_ssd2677_t *d)
{
    const int64_t deadline = esp_timer_get_time() + 30000000;
    delay_ms(1); /* Allow BUSY to assert after a command. BUSY is active LOW. */
    while (!gpio_get_level(d->busy_pin)) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        delay_ms(2);
    }
    return ESP_OK;
}

static esp_err_t set_power(epd_ssd2677_t *d, bool on)
{
    if (d->set_power) TRY(d->set_power(d->power_context, on));
    if (d->power_pin >= 0)
        TRY(gpio_set_level(d->power_pin, on ? d->power_active_level : !d->power_active_level));
    d->powered = on;
    if (on) delay_ms(20);
    return ESP_OK;
}

static esp_err_t powerdown(epd_ssd2677_t *d)
{
    if (!d->analog_on) return ESP_OK;
    TRY(command(d, 0x02, (uint8_t[]){0}, 1));
    TRY(wait_ready(d));
    d->analog_on = false;
    return ESP_OK;
}

static esp_err_t controller_init(epd_ssd2677_t *d)
{
    d->controller_ready = d->shadow_valid = d->analog_on = false;
    TRY(set_power(d, true));
    TRY(gpio_set_level(d->reset_pin, 0));
    delay_ms(20);
    TRY(gpio_set_level(d->reset_pin, 1));
    delay_ms(50);
    TRY(wait_ready(d));
    TRY(command(d, 0x00, (uint8_t[]){0x2f, 0x0e}, 2));
    TRY(wait_ready(d));
    TRY(command(d, 0x06, (uint8_t[]){0x0f, 0x8b, 0x93, 0xc1}, 4));
    TRY(command(d, 0xe7, (uint8_t[]){0xc1}, 1));
    TRY(command(d, 0x30, (uint8_t[]){0x08}, 1));
    TRY(command(d, 0x50, (uint8_t[]){0x77}, 1));
    TRY(command(d, 0x62, (uint8_t[]){0x76,0x76,0x76,0x5a,0x9d,0x8a,0x76,0x62}, 8));
    /* 680 gates are scanned; only 480 rows of image data are visible. */
    TRY(command(d, 0x61, (uint8_t[]){0x03,0x20,0x02,0xa8}, 4));
    TRY(command(d, 0xe0, (uint8_t[]){0x10}, 1));
    TRY(command(d, 0x65, (uint8_t[4]){0}, 4));
    TRY(command(d, 0xe9, (uint8_t[]){0x01}, 1));
    TRY(wait_ready(d));
    d->controller_ready = true;
    d->partial_refresh_count = 0;
    return ESP_OK;
}

static esp_err_t latch_waveform(epd_ssd2677_t *d, bool partial)
{
    TRY(command(d, 0x40, NULL, 0));
    TRY(wait_ready(d));
    uint8_t temperature = 0;
    TRY(gpio_set_level(d->dc_pin, 1));
    spi_transaction_t rx = {.rxlength = 8, .rx_buffer = &temperature, .override_freq_hz = 1000000};
    TRY(spi_device_polling_transmit(d->spi, &rx));
    /* Some panels have no working readback. Match Seeed's 25 C fallback. */
    d->temperature = temperature && temperature <= 127 ? temperature : 25;
    const unsigned bucket = d->temperature <= 10 ? 0 : d->temperature <= 20 ? 1 :
        d->temperature <= 30 ? 2 : 3;
    static const uint8_t full[] = {0xe8,0xeb,0xee,0xf1};
    static const uint8_t fast[] = {0x08,0x0f,0x19,0x23};
    const uint8_t waveform = partial ? fast[bucket] : full[bucket];
    TRY(command(d, 0xe0, (uint8_t[]){0x12}, 1));
    TRY(command(d, 0xe6, &waveform, 1));
    TRY(command(d, 0xa5, NULL, 0));
    TRY(wait_ready(d));
    delay_ms(10);
    return ESP_OK;
}

static bool pixel_white(const uint8_t *image, unsigned x, unsigned y)
{
    /* The V2 gate scan runs opposite to the SSD1677 profile. Reverse rows
     * while preserving source X, logical orientation and GT911 calibration. */
    y = SSD2677_HEIGHT - 1U - y;
    return (image[(y / 8U) * SSD2677_WIDTH + x] & (1U << (y % 8U))) != 0;
}

static esp_err_t write_image(epd_ssd2677_t *d, const uint8_t *image, bool partial)
{
    TRY(command(d, 0x10, NULL, 0));
    TRY(gpio_set_level(d->dc_pin, 1));
    for (unsigned y = 0; y < SSD2677_HEIGHT; y++) {
        for (unsigned x = 0; x < SSD2677_WIDTH; x += 4) {
            uint8_t packed = 0;
            for (unsigned bit = 0; bit < 4; bit++) {
                const bool white = pixel_white(image, x + bit, y);
                const bool old = partial ? pixel_white(d->shadow, x + bit, y) : white;
                packed |= (uint8_t)(((old << 1U) | white) << (6U - bit * 2U));
            }
            d->line_buffer[x / 4U] = packed;
        }
        spi_transaction_t tx = {.length = SSD2677_ROW_BYTES * 8U, .tx_buffer = d->line_buffer};
        TRY(spi_device_polling_transmit(d->spi, &tx));
    }
    return ESP_OK;
}

static esp_err_t refresh_image(epd_ssd2677_t *d, const uint8_t *image, bool force_full)
{
    if (!d->controller_ready) TRY(controller_init(d));
    if (!force_full && d->shadow_valid && d->shadow &&
        !memcmp(image, d->shadow, SSD2677_BUFFER_BYTES)) return ESP_OK;
    const bool partial = !force_full && d->shadow_valid && d->shadow &&
        d->refresh_mode != EPD_SSD2677_REFRESH_FULL &&
        (d->refresh_mode == EPD_SSD2677_REFRESH_PARTIAL || d->partial_refresh_count < 19U);
    TRY(latch_waveform(d, partial));
    /* SSD2677 has no rectangular RAM window: partial is a full-frame diff. */
    TRY(write_image(d, image, partial));
    if (!d->analog_on) {
        /* Mark possibly-on before transmission so failure cleanup powers off. */
        d->analog_on = true;
        TRY(command(d, 0x04, NULL, 0));
        TRY(wait_ready(d));
    }
    TRY(command(d, 0x12, (uint8_t[]){0}, 1));
    TRY(wait_ready(d));
    if (!partial) TRY(powerdown(d));
    if (d->shadow) memcpy(d->shadow, image, SSD2677_BUFFER_BYTES);
    d->shadow_valid = d->shadow != NULL;
    d->partial_refresh_count = partial ? d->partial_refresh_count + 1U : 0;
    return ESP_OK;
}

static void invalidate(epd_ssd2677_t *d, esp_err_t err)
{
    d->last_error = err;
    if (err != ESP_OK) d->controller_ready = d->shadow_valid = false;
}

static esp_err_t refresh(epd_ssd2677_t *d)
{
    if (!d || !d->spi || !d->buffer || !d->line_buffer) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(d->lock, portMAX_DELAY);
    esp_err_t err = d->suspended || d->power_state_present ? ESP_ERR_INVALID_STATE :
        d->relax.storage ? ESP_OK : refresh_image(d, d->buffer, false);
    invalidate(d, err);
    xSemaphoreGive(d->lock);
    return err;
}

static esp_err_t sleep_panel(epd_ssd2677_t *d)
{
    esp_err_t err = ESP_OK;
    if (d->powered) {
        err = powerdown(d);
        if (err == ESP_OK && d->controller_ready)
            err = command(d, 0x07, (uint8_t[]){0xa5}, 1);
        (void)gpio_set_level(d->reset_pin, 0);
        (void)gpio_set_level(d->dc_pin, 0);
        const esp_err_t power = set_power(d, false);
        if (err == ESP_OK) err = power;
    }
    d->suspended = true;
    d->controller_ready = d->shadow_valid = d->analog_on = false;
    return err;
}

static const u8x8_display_info_t display_info = {
    .chip_enable_level = 0, .chip_disable_level = 1,
    .reset_pulse_width_ms = 20, .post_reset_wait_ms = 50,
    .sck_clock_hz = 20000000, .spi_mode = 0,
    .tile_width = SSD2677_WIDTH / 8U, .tile_height = SSD2677_HEIGHT / 8U,
    .pixel_width = SSD2677_WIDTH, .pixel_height = SSD2677_HEIGHT,
};

static uint8_t display_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg, void *ptr)
{
    (void)ptr;
    if (msg == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(u8x8, &display_info);
        return 1;
    }
    epd_ssd2677_t *d = (epd_ssd2677_t *)((uint8_t *)u8x8 -
        offsetof(epd_ssd2677_t, u8g2) - offsetof(u8g2_t, u8x8));
    if (msg == U8X8_MSG_DISPLAY_DRAW_TILE) return 1;
    if (msg == U8X8_MSG_DISPLAY_REFRESH) return refresh(d) == ESP_OK;
    xSemaphoreTake(d->lock, portMAX_DELAY);
    esp_err_t err;
    if (msg == U8X8_MSG_DISPLAY_INIT) err = controller_init(d);
    else if (msg == U8X8_MSG_DISPLAY_SET_POWER_SAVE) {
        if (arg) err = sleep_panel(d);
        else {
            d->suspended = d->power_state_present = false;
            err = d->controller_ready ? ESP_OK : controller_init(d);
        }
    } else { xSemaphoreGive(d->lock); return 0; }
    invalidate(d, err);
    xSemaphoreGive(d->lock);
    return err == ESP_OK;
}

esp_err_t epd_ssd2677_init(epd_ssd2677_t *d, const epd_ssd2677_config_t *c)
{
    if (!d || !c || !c->rotation || c->spi_clock_hz < 100000 || c->spi_clock_hz > 20000000 ||
        c->busy_level != 0 || c->panel > EPD_SSD2677_PANEL_STICKY ||
        (c->power_active_level != 0 && c->power_active_level != 1)) return ESP_ERR_INVALID_ARG;
    const int pins[] = {c->cs_pin,c->dc_pin,c->reset_pin,c->busy_pin,c->power_pin};
    for (unsigned i = 0; i < 5; i++) {
        if (i == 4 && pins[i] == -1) continue;
        if (pins[i] < 0 || pins[i] >= GPIO_NUM_MAX ||
            (i == 3 ? !GPIO_IS_VALID_GPIO(pins[i]) : !GPIO_IS_VALID_OUTPUT_GPIO(pins[i])))
            return ESP_ERR_INVALID_ARG;
        for (unsigned j = 0; j < i; j++) if (pins[j] == pins[i]) return ESP_ERR_INVALID_ARG;
    }
    if ((!c->spi_bus || !c->spi_bus[0]) &&
        (!GPIO_IS_VALID_OUTPUT_GPIO(c->sclk_pin) || !GPIO_IS_VALID_OUTPUT_GPIO(c->mosi_pin) ||
         (c->spi_host != SPI2_HOST && c->spi_host != SPI3_HOST))) return ESP_ERR_INVALID_ARG;
    memset(d, 0, sizeof(*d));
    d->dc_pin = c->dc_pin; d->reset_pin = c->reset_pin; d->busy_pin = c->busy_pin;
    d->power_pin = c->power_pin; d->power_active_level = c->power_active_level;
    d->set_power = c->set_power; d->power_context = c->power_context; d->spi_host = c->spi_host;
    d->lock = xSemaphoreCreateMutexStatic(&d->lock_storage);
    if (!d->lock) return ESP_ERR_NO_MEM;
    uint64_t outputs = (1ULL << c->dc_pin) | (1ULL << c->reset_pin);
    if (c->power_pin >= 0) outputs |= 1ULL << c->power_pin;
    gpio_config_t gpio = {.pin_bit_mask = outputs, .mode = GPIO_MODE_OUTPUT};
    esp_err_t err = gpio_config(&gpio);
    gpio = (gpio_config_t){.pin_bit_mask = 1ULL << c->busy_pin, .mode = GPIO_MODE_INPUT};
    if (err == ESP_OK) err = gpio_config(&gpio);
    if (err == ESP_OK) err = set_power(d, false);
    const spi_device_interface_config_t device = {
        .clock_speed_hz = c->spi_clock_hz, .mode = 0, .spics_io_num = c->cs_pin, .queue_size = 1,
        .flags = SPI_DEVICE_3WIRE | SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY,
    };
    if (err == ESP_OK && c->spi_bus && c->spi_bus[0])
        err = solar_os_bus_spi_add_device(c->spi_bus, &device, &d->spi);
    else if (err == ESP_OK) {
        const spi_bus_config_t bus = {.mosi_io_num = c->mosi_pin, .miso_io_num = -1,
            .sclk_io_num = c->sclk_pin, .quadwp_io_num = -1, .quadhd_io_num = -1,
            .max_transfer_sz = SSD2677_ROW_BYTES};
        err = spi_bus_initialize(c->spi_host, &bus, SPI_DMA_CH_AUTO);
        if (err == ESP_OK) { d->bus_initialized = true; err = spi_bus_add_device(c->spi_host, &device, &d->spi); }
    }
    if (err == ESP_OK) {
        d->buffer_size = d->shadow_size = SSD2677_BUFFER_BYTES;
        d->line_buffer_size = SSD2677_ROW_BYTES;
        d->buffer = solar_os_memory_calloc(1, d->buffer_size,
            SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "ssd2677.frame");
        d->shadow = solar_os_memory_alloc(d->shadow_size,
            SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "ssd2677.shadow");
        d->line_buffer = solar_os_memory_alloc(d->line_buffer_size,
            SOLAR_OS_MEMORY_DMA, "ssd2677.spi");
        if (!d->buffer || !d->line_buffer) err = ESP_ERR_NO_MEM;
        if (!d->shadow) d->shadow_size = 0;
    }
    if (err == ESP_OK) {
        u8g2_SetupDisplay(&d->u8g2, display_cb, u8x8_dummy_cb, u8x8_dummy_cb, u8x8_dummy_cb);
        u8g2_SetupBuffer(&d->u8g2, d->buffer, SSD2677_HEIGHT / 8U,
            u8g2_ll_hvline_vertical_top_lsb, c->rotation);
        u8g2_InitDisplay(&d->u8g2);
        err = d->last_error;
    }
    if (err != ESP_OK) epd_ssd2677_deinit(d);
    return err;
}

void epd_ssd2677_deinit(epd_ssd2677_t *d)
{
    /* A rejected configuration leaves the service's zeroed instance untouched;
     * cleanup must not reset GPIO0 from that uninitialized instance. */
    if (!d || !d->lock) return;
    if (d->spi) {
        if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
        (void)sleep_panel(d);
        if (d->lock) xSemaphoreGive(d->lock);
        (void)spi_bus_remove_device(d->spi);
        d->spi = NULL;
    }
    if (d->bus_initialized) { (void)spi_bus_free(d->spi_host); d->bus_initialized = false; }
    epd_relax_dispose(&d->relax);
    solar_os_memory_free(d->buffer); solar_os_memory_free(d->shadow); solar_os_memory_free(d->line_buffer);
    d->buffer = d->shadow = d->line_buffer = NULL;
    d->buffer_size = d->shadow_size = d->line_buffer_size = 0;
    (void)gpio_reset_pin(d->dc_pin); (void)gpio_reset_pin(d->reset_pin);
    (void)gpio_reset_pin(d->busy_pin);
    if (d->power_pin >= 0) (void)gpio_reset_pin(d->power_pin);
    d->lock = NULL;
}

esp_err_t epd_ssd2677_resume(epd_ssd2677_t *d)
{
    if (!d || !d->spi || !d->buffer) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(d->lock, portMAX_DELAY);
    d->suspended = d->power_state_present = false;
    const esp_err_t err = controller_init(d);
    invalidate(d, err);
    xSemaphoreGive(d->lock);
    return err;
}

u8g2_t *epd_ssd2677_get_u8g2(epd_ssd2677_t *d) { return d ? &d->u8g2 : NULL; }

static uint8_t reverse_byte(uint8_t value)
{
    value = ((value & 0x55U) << 1U) | ((value >> 1U) & 0x55U);
    value = ((value & 0x33U) << 2U) | ((value >> 2U) & 0x33U);
    return (value << 4U) | (value >> 4U);
}

static void transpose_block(uint32_t *low, uint32_t *high)
{
    uint32_t a = *low, b = *high;
    uint32_t swap = (a ^ (a >> 7U)) & 0x00aa00aaU;
    a ^= swap ^ (swap << 7U);
    swap = (b ^ (b >> 7U)) & 0x00aa00aaU;
    b ^= swap ^ (swap << 7U);
    swap = (a ^ (a >> 14U)) & 0x0000ccccU;
    a ^= swap ^ (swap << 14U);
    swap = (b ^ (b >> 14U)) & 0x0000ccccU;
    b ^= swap ^ (swap << 14U);
    swap = (a ^ (b << 4U)) & 0xf0f0f0f0U;
    *low = a ^ swap;
    *high = b ^ (swap >> 4U);
}

static bool blit_mono_packed(epd_ssd2677_t *d, const uint8_t *bitmap,
    unsigned x, unsigned y, unsigned width, unsigned height, unsigned stride,
    const u8g2_cb_t *rotation, bool inverted)
{
    if (rotation == U8G2_R0 || rotation == U8G2_R2) {
        if ((y | height) & 7U) return false;
        for (unsigned row = 0; row < height; row += 8U) {
            const unsigned native_y = rotation == U8G2_R0 ? y + row :
                SSD2677_HEIGHT - y - row - 8U;
            uint8_t *out = d->buffer + (native_y / 8U) * SSD2677_WIDTH;
            for (unsigned column = 0; column < width; column += 8U) {
                const uint8_t *source = bitmap + row * stride + column / 8U;
                uint32_t low = (uint32_t)source[0] | ((uint32_t)source[stride] << 8U) |
                    ((uint32_t)source[2U * stride] << 16U) | ((uint32_t)source[3U * stride] << 24U);
                uint32_t high = (uint32_t)source[4U * stride] | ((uint32_t)source[5U * stride] << 8U) |
                    ((uint32_t)source[6U * stride] << 16U) | ((uint32_t)source[7U * stride] << 24U);
                transpose_block(&low, &high);
                const unsigned count = width - column < 8U ? width - column : 8U;
                for (unsigned i = 0; i < count; i++) {
                    uint8_t value = (uint8_t)((i < 4U ? low : high) >> ((i & 3U) * 8U));
                    const unsigned nx = rotation == U8G2_R0 ? x + column + i :
                        SSD2677_WIDTH - 1U - x - column - i;
                    if (rotation == U8G2_R2) value = reverse_byte(value);
                    out[nx] = inverted ? value : (uint8_t)~value;
                }
            }
        }
    } else {
        if ((x | width) & 7U) return false;
        for (unsigned row = 0; row < height; row++) {
            const uint8_t *source = bitmap + row * stride;
            const unsigned nx = rotation == U8G2_R1 ? SSD2677_WIDTH - 1U - y - row : y + row;
            for (unsigned column = 0; column < width; column += 8U) {
                const unsigned ny = rotation == U8G2_R1 ? x + column : SSD2677_HEIGHT - x - column - 8U;
                uint8_t value = source[column / 8U];
                if (rotation == U8G2_R3) value = reverse_byte(value);
                d->buffer[(ny / 8U) * SSD2677_WIDTH + nx] = inverted ? value : (uint8_t)~value;
            }
        }
    }
    return true;
}

esp_err_t epd_ssd2677_present_mono_xbm(epd_ssd2677_t *d,
    const uint8_t *bitmap, size_t bitmap_size, uint16_t x, uint16_t y,
    uint16_t width, uint16_t height, uint16_t stride, bool palette_inverted)
{
    if (!d || !d->buffer || !bitmap || !width || !height ||
        stride < ((size_t)width + 7U) / 8U || height > SIZE_MAX / stride ||
        bitmap_size < (size_t)height * stride ||
        (uint32_t)x + width > u8g2_GetDisplayWidth(&d->u8g2) ||
        (uint32_t)y + height > u8g2_GetDisplayHeight(&d->u8g2)) return ESP_ERR_INVALID_ARG;
    const u8g2_cb_t *rotation = d->u8g2.cb;
    if (rotation != U8G2_R0 && rotation != U8G2_R1 &&
        rotation != U8G2_R2 && rotation != U8G2_R3) return ESP_ERR_NOT_SUPPORTED;
    xSemaphoreTake(d->lock, portMAX_DELAY);
    if (!d->powered || d->suspended || d->power_state_present || d->relax.storage ||
        !d->spi || !d->line_buffer || d->buffer_size < SSD2677_BUFFER_BYTES) {
        xSemaphoreGive(d->lock);
        return ESP_ERR_INVALID_STATE;
    }
    memset(d->buffer, palette_inverted ? 0 : 0xff, SSD2677_BUFFER_BYTES);
    if (!blit_mono_packed(d, bitmap, x, y, width, height, stride, rotation, palette_inverted)) {
        for (unsigned row = 0; row < height; row++) {
            const uint8_t *source = bitmap + (size_t)row * stride;
            for (unsigned column = 0; column < width; column++) {
                const unsigned lx = x + column, ly = y + row;
                unsigned nx = lx, ny = ly;
                if (rotation == U8G2_R1) { nx = SSD2677_WIDTH - 1U - ly; ny = lx; }
                else if (rotation == U8G2_R2) {
                    nx = SSD2677_WIDTH - 1U - lx; ny = SSD2677_HEIGHT - 1U - ly;
                } else if (rotation == U8G2_R3) { nx = ly; ny = SSD2677_HEIGHT - 1U - lx; }
                uint8_t *destination = d->buffer + (ny / 8U) * SSD2677_WIDTH + nx;
                const uint8_t mask = (uint8_t)(1U << (ny & 7U));
                const bool black = (source[column / 8U] & (1U << (column & 7U))) != 0;
                if (black == palette_inverted) *destination |= mask;
                else *destination &= (uint8_t)~mask;
            }
        }
    }
    const esp_err_t err = refresh_image(d, d->buffer, false);
    invalidate(d, err);
    xSemaphoreGive(d->lock);
    return err;
}

esp_err_t epd_ssd2677_present_power_state(epd_ssd2677_t *d)
{
    if (!d || !d->spi || !d->buffer || !d->line_buffer) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(d->lock, portMAX_DELAY);
    d->power_state_present = true;
    const esp_err_t err = d->suspended ? ESP_ERR_INVALID_STATE : refresh_image(d, d->buffer, true);
    invalidate(d, err);
    xSemaphoreGive(d->lock);
    return err;
}

static esp_err_t relax_native(void *context, solar_os_display_relax_phase_t phase, const uint8_t *image)
{
    epd_ssd2677_t *d = context;
    if (!d->powered || d->suspended || d->power_state_present) return ESP_ERR_INVALID_STATE;
    return phase == SOLAR_OS_DISPLAY_RELAX_BEGIN ? powerdown(d) : refresh_image(d, image, true);
}

esp_err_t epd_ssd2677_relax(epd_ssd2677_t *d, solar_os_display_relax_phase_t phase)
{
    if (!d || !d->buffer || !d->spi || (unsigned)phase > SOLAR_OS_DISPLAY_RELAX_END)
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(d->lock, portMAX_DELAY);
    const esp_err_t err = phase == SOLAR_OS_DISPLAY_RELAX_BEGIN &&
        (!d->powered || d->suspended || d->power_state_present) ? ESP_ERR_INVALID_STATE :
        epd_relax_apply(&d->relax, d->buffer, d->buffer_size, phase, relax_native, d);
    invalidate(d, err);
    xSemaphoreGive(d->lock);
    return err;
}

const char *epd_ssd2677_controller_mode(const epd_ssd2677_t *display)
{
    if (!display) return NULL;
    epd_ssd2677_t *d = (epd_ssd2677_t *)display;
    xSemaphoreTake(d->lock, portMAX_DELAY);
    snprintf(d->mode_description, sizeof(d->mode_description),
        "refresh=%s waveform=otp temperature=%u partial-count=%u",
        d->refresh_mode == EPD_SSD2677_REFRESH_FULL ? "full" :
        d->refresh_mode == EPD_SSD2677_REFRESH_PARTIAL ? "partial" : "auto",
        d->temperature, d->partial_refresh_count);
    xSemaphoreGive(d->lock);
    return d->mode_description;
}

const char *epd_ssd2677_controller_mode_values(const epd_ssd2677_t *d)
{
    (void)d;
    return "refresh=<auto,fast,partial,full> waveform=<otp>";
}

esp_err_t epd_ssd2677_set_controller_mode(epd_ssd2677_t *d, const char *mode)
{
    if (!d || !mode) return ESP_ERR_INVALID_ARG;
    epd_ssd2677_refresh_mode_t refresh_mode;
    if (!strcmp(mode, "refresh=auto") || !strcmp(mode, "waveform=otp")) refresh_mode = EPD_SSD2677_REFRESH_AUTO;
    else if (!strcmp(mode, "refresh=fast") || !strcmp(mode, "refresh=partial")) refresh_mode = EPD_SSD2677_REFRESH_PARTIAL;
    else if (!strcmp(mode, "refresh=full")) refresh_mode = EPD_SSD2677_REFRESH_FULL;
    else if (!strcmp(mode, "refresh=pulse") || !strcmp(mode, "waveform=fastwave")) return ESP_ERR_NOT_SUPPORTED;
    else return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(d->lock, portMAX_DELAY);
    const esp_err_t err = d->relax.storage || d->suspended || d->power_state_present ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (err == ESP_OK) d->refresh_mode = refresh_mode;
    xSemaphoreGive(d->lock);
    return err;
}

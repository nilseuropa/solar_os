#include "epd_ssd1680.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "solar_os_buses.h"
#include "solar_os_memory.h"

#define BUSY_TIMEOUT_US 30000000LL
#define PARTIAL_CLEANUP_EVERY 5U
#define TRY(call) do { esp_err_t result_ = (call); if (result_ != ESP_OK) return result_; } while (0)

static void delay_ms(unsigned ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

static esp_err_t command(epd_ssd1680_t *d, uint8_t cmd, const uint8_t *data, size_t length)
{
    TRY(gpio_set_level(d->dc_pin, 0));
    spi_transaction_t transaction = {.flags = SPI_TRANS_USE_TXDATA, .length = 8, .tx_data = {cmd}};
    TRY(spi_device_polling_transmit(d->spi, &transaction));
    if (!length) return ESP_OK;
    TRY(gpio_set_level(d->dc_pin, 1));
    while (length) {
        const size_t chunk = length < EPD_SSD1680_ROW_BYTES ? length : EPD_SSD1680_ROW_BYTES;
        memcpy(d->line_buffer, data, chunk);
        transaction = (spi_transaction_t){.length = chunk * 8U, .tx_buffer = d->line_buffer};
        TRY(spi_device_polling_transmit(d->spi, &transaction));
        data += chunk;
        length -= chunk;
    }
    return ESP_OK;
}

static esp_err_t wait_ready(epd_ssd1680_t *d)
{
    const int64_t deadline = esp_timer_get_time() + BUSY_TIMEOUT_US;
    /* BUSY is active high and can assert shortly after master activation. */
    delay_ms(10);
    while (gpio_get_level(d->busy_pin)) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        delay_ms(10);
    }
    return ESP_OK;
}

static esp_err_t external_power(epd_ssd1680_t *d, bool on)
{
    if (d->power_pin >= 0) TRY(gpio_set_level(d->power_pin, on));
    d->powered = on;
    if (on) delay_ms(20);
    return ESP_OK;
}

static esp_err_t set_window(epd_ssd1680_t *d)
{
    TRY(command(d, 0x11, (uint8_t[]){3}, 1));
    TRY(command(d, 0x44, (uint8_t[]){0, EPD_SSD1680_ROW_BYTES - 1U}, 2));
    TRY(command(d, 0x45, (uint8_t[]){0, 0, EPD_SSD1680_HEIGHT - 1U, 0}, 4));
    TRY(command(d, 0x4e, (uint8_t[]){0}, 1));
    return command(d, 0x4f, (uint8_t[]){0, 0}, 2);
}

/* Register sequence follows Waveshare's EPD_2in13_V4 reference. The profile
 * uses its panel OTP; SSD1680 glass requiring a RAM LUT needs a separate profile.
 * https://github.com/waveshareteam/e-Paper/blob/master/RaspberryPi_JetsonNano/c/lib/e-Paper/EPD_2in13_V4.c
 */
static esp_err_t controller_init(epd_ssd1680_t *d)
{
    d->controller_ready = d->shadow_valid = false;
    d->partial_count = 0;
    TRY(external_power(d, true));
    TRY(gpio_set_level(d->reset_pin, 1)); delay_ms(20);
    TRY(gpio_set_level(d->reset_pin, 0)); delay_ms(2);
    TRY(gpio_set_level(d->reset_pin, 1)); delay_ms(20);
    TRY(wait_ready(d));
    TRY(command(d, 0x12, NULL, 0));
    TRY(wait_ready(d));
    TRY(command(d, 0x01, (uint8_t[]){EPD_SSD1680_HEIGHT - 1U, 0, 0}, 3));
    TRY(command(d, 0x3c, (uint8_t[]){5}, 1));
    TRY(command(d, 0x21, (uint8_t[]){0, 0x80}, 2));
    TRY(command(d, 0x18, (uint8_t[]){0x80}, 1));
    TRY(set_window(d));
    d->controller_ready = true;
    return ESP_OK;
}

/* U8g2 stores vertical tiles in 128-byte bands, including six invisible
 * columns and six invisible rows. Panel RAM takes only 250 horizontal rows;
 * every row's six unused low bits are kept white, even for an all-black frame.
 */
static esp_err_t write_plane(epd_ssd1680_t *d, uint8_t cmd, const uint8_t *frame)
{
    TRY(set_window(d));
    TRY(command(d, cmd, NULL, 0));
    TRY(gpio_set_level(d->dc_pin, 1));
    TRY(spi_device_acquire_bus(d->spi, portMAX_DELAY));
    esp_err_t err = ESP_OK;
    for (unsigned y = 0; y < EPD_SSD1680_HEIGHT && err == ESP_OK; y++) {
        memset(d->line_buffer, 0xff, EPD_SSD1680_ROW_BYTES);
        const uint8_t *tile = frame + (y / 8U) * EPD_SSD1680_BUFFER_STRIDE;
        for (unsigned x = 0; x < EPD_SSD1680_WIDTH; x++) {
            if ((tile[x] & (1U << (y % 8U))) == 0)
                d->line_buffer[x / 8U] &= (uint8_t)~(0x80U >> (x % 8U));
        }
        spi_transaction_t transaction = {
            .flags = y + 1U < EPD_SSD1680_HEIGHT ? SPI_TRANS_CS_KEEP_ACTIVE : 0,
            .length = EPD_SSD1680_ROW_BYTES * 8U, .tx_buffer = d->line_buffer,
        };
        err = spi_device_polling_transmit(d->spi, &transaction);
    }
    if (err != ESP_OK) {
        spi_transaction_t end = {.length = 0};
        (void)spi_device_polling_transmit(d->spi, &end);
    }
    spi_device_release_bus(d->spi);
    return err;
}

static esp_err_t native_frame(epd_ssd1680_t *d, const uint8_t *image, bool partial)
{
    if (!d->controller_ready) {
        TRY(controller_init(d));
        partial = false;
    }
    if (partial) {
        /* The V4 partial sequence resets control registers without SWRESET. */
        TRY(gpio_set_level(d->reset_pin, 0)); delay_ms(1);
        TRY(gpio_set_level(d->reset_pin, 1)); delay_ms(20);
        TRY(wait_ready(d));
        TRY(command(d, 0x01, (uint8_t[]){EPD_SSD1680_HEIGHT - 1U, 0, 0}, 3));
        TRY(command(d, 0x21, (uint8_t[]){0, 0x80}, 2));
    }
    TRY(command(d, 0x3c, (uint8_t[]){partial ? 0x80 : 5}, 1));
    TRY(write_plane(d, 0x26, partial ? d->shadow : image));
    TRY(write_plane(d, 0x24, image));
    TRY(command(d, 0x22, (uint8_t[]){partial ? 0xff : 0xf7}, 1));
    TRY(command(d, 0x20, NULL, 0));
    /* Never retain ownership of the shared SPI bus while waiting for glass. */
    TRY(wait_ready(d));
    TRY(write_plane(d, 0x26, image));
    memcpy(d->shadow, image, EPD_SSD1680_BUFFER_BYTES);
    d->shadow_valid = true;
    d->partial_count = partial ? d->partial_count + 1U : 0;
    return ESP_OK;
}

static bool image_changed(const epd_ssd1680_t *d)
{
    if (!d->shadow_valid) return true;
    if (memcmp(d->buffer, d->shadow, EPD_SSD1680_BUFFER_BYTES) == 0) return false;
    for (unsigned band = 0; band < EPD_SSD1680_TILE_ROWS; band++) {
        const unsigned rows = EPD_SSD1680_HEIGHT - band * 8U;
        const uint8_t mask = rows < 8U ? (uint8_t)((1U << rows) - 1U) : 0xff;
        for (unsigned x = 0; x < EPD_SSD1680_WIDTH; x++) {
            const size_t offset = band * EPD_SSD1680_BUFFER_STRIDE + x;
            if ((d->buffer[offset] ^ d->shadow[offset]) & mask) return true;
        }
    }
    return false;
}

static esp_err_t record_error(epd_ssd1680_t *d, esp_err_t err)
{
    d->last_error = err;
    if (err != ESP_OK) d->controller_ready = d->shadow_valid = false;
    return err;
}

static esp_err_t refresh_locked(epd_ssd1680_t *d, bool force_full)
{
    if (d->suspended || !d->spi || !d->buffer || !d->shadow || !d->line_buffer)
        return ESP_ERR_INVALID_STATE;
    if (d->relax.storage || (!force_full && !image_changed(d))) return ESP_OK;
    const bool partial = !force_full && d->shadow_valid &&
        d->refresh_mode != EPD_SSD1680_REFRESH_FULL && d->partial_count < PARTIAL_CLEANUP_EVERY;
    return record_error(d, native_frame(d, d->buffer, partial));
}

esp_err_t epd_ssd1680_refresh(epd_ssd1680_t *d)
{
    if (!d) return ESP_ERR_INVALID_STATE;
    if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
    esp_err_t err = refresh_locked(d, false);
    if (d->lock) xSemaphoreGive(d->lock);
    return err;
}

esp_err_t epd_ssd1680_present_power_state(epd_ssd1680_t *d)
{
    if (!d) return ESP_ERR_INVALID_STATE;
    if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
    esp_err_t err = d->relax.storage ? ESP_ERR_INVALID_STATE : refresh_locked(d, true);
    if (d->lock) xSemaphoreGive(d->lock);
    return err;
}

esp_err_t epd_ssd1680_present_mono_xbm(epd_ssd1680_t *d,
    const uint8_t *bitmap, size_t bitmap_size, uint16_t x, uint16_t y,
    uint16_t width, uint16_t height, uint16_t stride, bool inverted)
{
    if (!d || !d->buffer || !bitmap || !width || !height ||
        stride < ((size_t)width + 7U) / 8U || height > SIZE_MAX / stride ||
        bitmap_size < (size_t)height * stride ||
        (uint32_t)x + width > u8g2_GetDisplayWidth(&d->u8g2) ||
        (uint32_t)y + height > u8g2_GetDisplayHeight(&d->u8g2)) return ESP_ERR_INVALID_ARG;
    const u8g2_cb_t *rotation = d->u8g2.cb;
    if (rotation != U8G2_R0 && rotation != U8G2_R1 &&
        rotation != U8G2_R2 && rotation != U8G2_R3) return ESP_ERR_NOT_SUPPORTED;
    if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!d->suspended && !d->relax.storage && d->spi && d->shadow && d->line_buffer) {
        memset(d->buffer, inverted ? 0 : 0xff, EPD_SSD1680_BUFFER_BYTES);
        for (unsigned row = 0; row < height; row++) {
            const uint8_t *source = bitmap + (size_t)row * stride;
            for (unsigned column = 0; column < width; column++) {
                const unsigned lx = x + column, ly = y + row;
                unsigned nx = lx, ny = ly;
                if (rotation == U8G2_R1) { nx = EPD_SSD1680_WIDTH - 1U - ly; ny = lx; }
                else if (rotation == U8G2_R2) {
                    nx = EPD_SSD1680_WIDTH - 1U - lx; ny = EPD_SSD1680_HEIGHT - 1U - ly;
                } else if (rotation == U8G2_R3) { nx = ly; ny = EPD_SSD1680_HEIGHT - 1U - lx; }
                uint8_t *destination = d->buffer + (ny / 8U) * EPD_SSD1680_BUFFER_STRIDE + nx;
                const uint8_t mask = (uint8_t)(1U << (ny & 7U));
                const bool black = (source[column / 8U] & (1U << (column & 7U))) != 0;
                if (black == inverted) *destination |= mask;
                else *destination &= (uint8_t)~mask;
            }
        }
        err = refresh_locked(d, false);
    }
    if (d->lock) xSemaphoreGive(d->lock);
    return err;
}

static esp_err_t sleep_panel(epd_ssd1680_t *d)
{
    d->suspended = true;
    esp_err_t err = d->powered ? command(d, 0x10, (uint8_t[]){1}, 1) : ESP_OK;
    if (d->powered) delay_ms(100);
    const esp_err_t power_err = external_power(d, false);
    d->controller_ready = d->shadow_valid = false;
    return err != ESP_OK ? err : power_err;
}

static const u8x8_display_info_t display_info = {
    .chip_enable_level = 0, .chip_disable_level = 1,
    .reset_pulse_width_ms = 2, .post_reset_wait_ms = 20,
    .sck_clock_hz = 4000000, .spi_mode = 0,
    .tile_width = EPD_SSD1680_ROW_BYTES, .tile_height = EPD_SSD1680_TILE_ROWS,
    .pixel_width = EPD_SSD1680_WIDTH, .pixel_height = EPD_SSD1680_HEIGHT,
};

static uint8_t display_cb(u8x8_t *u8x8, uint8_t message, uint8_t arg, void *ptr)
{
    (void)ptr;
    if (message == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(u8x8, &display_info);
        return 1;
    }
    epd_ssd1680_t *d = (epd_ssd1680_t *)((uint8_t *)u8x8 -
        offsetof(epd_ssd1680_t, u8g2) - offsetof(u8g2_t, u8x8));
    if (message == U8X8_MSG_DISPLAY_DRAW_TILE) return 1;
    if (message == U8X8_MSG_DISPLAY_REFRESH) return epd_ssd1680_refresh(d) == ESP_OK;
    if (message != U8X8_MSG_DISPLAY_INIT && message != U8X8_MSG_DISPLAY_SET_POWER_SAVE) return 0;
    if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
    esp_err_t err;
    if (message == U8X8_MSG_DISPLAY_SET_POWER_SAVE && arg) err = sleep_panel(d);
    else {
        d->suspended = false;
        err = d->controller_ready ? ESP_OK : controller_init(d);
    }
    record_error(d, err);
    if (d->lock) xSemaphoreGive(d->lock);
    return err == ESP_OK;
}

static bool config_valid(const epd_ssd1680_config_t *c)
{
    if (!c || !c->spi_bus || !c->spi_bus[0] ||
        (c->rotation != U8G2_R0 && c->rotation != U8G2_R1 &&
         c->rotation != U8G2_R2 && c->rotation != U8G2_R3) ||
        c->spi_clock_hz < 100000 || c->spi_clock_hz > 4000000 ||
        c->power_pin < -1 || c->panel != EPD_SSD1680_PANEL_122X250) return false;
    const int pins[] = {c->cs_pin, c->dc_pin, c->reset_pin, c->busy_pin, c->power_pin};
    for (unsigned i = 0; i < 5; i++) {
        if (i == 4 && pins[i] == -1) continue;
        if (pins[i] < 0 || pins[i] >= GPIO_NUM_MAX || pins[i] >= 64) return false;
        for (unsigned j = i + 1; j < 5; j++) if (pins[i] == pins[j]) return false;
    }
    return true;
}

static esp_err_t configure_pins(epd_ssd1680_t *d)
{
    const gpio_config_t output = {
        .pin_bit_mask = (1ULL << d->dc_pin) | (1ULL << d->reset_pin) |
            (d->power_pin >= 0 ? 1ULL << d->power_pin : 0),
        .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE,
    };
    d->pins_configured = true;
    TRY(gpio_config(&output));
    const gpio_config_t input = {
        .pin_bit_mask = 1ULL << d->busy_pin, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    TRY(gpio_config(&input));
    TRY(gpio_set_level(d->dc_pin, 1));
    return gpio_set_level(d->reset_pin, 1);
}

esp_err_t epd_ssd1680_init(epd_ssd1680_t *d, const epd_ssd1680_config_t *c)
{
    if (!d || !config_valid(c)) return ESP_ERR_INVALID_ARG;
    *d = (epd_ssd1680_t){.dc_pin = c->dc_pin, .reset_pin = c->reset_pin,
                        .busy_pin = c->busy_pin, .power_pin = c->power_pin};
    d->lock = xSemaphoreCreateMutexStatic(&d->lock_storage);
    if (!d->lock) return ESP_ERR_NO_MEM;
    esp_err_t err = configure_pins(d);
    if (err == ESP_OK) err = external_power(d, false);
    if (err == ESP_OK) {
        const spi_device_interface_config_t config = {
            .clock_speed_hz = c->spi_clock_hz, .mode = 0, .spics_io_num = c->cs_pin, .queue_size = 1,
        };
        err = solar_os_bus_spi_add_device(c->spi_bus, &config, &d->spi);
    }
    if (err == ESP_OK) {
        d->line_buffer = solar_os_memory_alloc(EPD_SSD1680_ROW_BYTES, SOLAR_OS_MEMORY_DMA, "ssd1680.spi");
        d->buffer = solar_os_memory_alloc(EPD_SSD1680_BUFFER_BYTES, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "ssd1680.frame");
        d->shadow = solar_os_memory_alloc(EPD_SSD1680_BUFFER_BYTES, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "ssd1680.shadow");
        if (!d->buffer || !d->shadow || !d->line_buffer) err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        memset(d->buffer, 0xff, EPD_SSD1680_BUFFER_BYTES);
        u8g2_SetupDisplay(&d->u8g2, display_cb, u8x8_dummy_cb, u8x8_dummy_cb, u8x8_dummy_cb);
        u8g2_SetupBuffer(&d->u8g2, d->buffer, EPD_SSD1680_TILE_ROWS,
                         u8g2_ll_hvline_vertical_top_lsb, c->rotation);
        u8g2_InitDisplay(&d->u8g2);
        err = d->last_error;
    }
    if (err != ESP_OK) epd_ssd1680_deinit(d);
    return err;
}

esp_err_t epd_ssd1680_resume(epd_ssd1680_t *d)
{
    if (!d || !d->spi || !d->buffer || !d->shadow || !d->line_buffer) return ESP_ERR_INVALID_STATE;
    if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
    d->suspended = false;
    esp_err_t err = configure_pins(d);
    if (err == ESP_OK) err = controller_init(d);
    record_error(d, err);
    if (d->lock) xSemaphoreGive(d->lock);
    return err;
}

void epd_ssd1680_deinit(epd_ssd1680_t *d)
{
    if (!d) return;
    if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
    if (d->spi && d->powered) (void)sleep_panel(d);
    if (d->spi) (void)spi_bus_remove_device(d->spi);
    solar_os_memory_free(d->line_buffer);
    solar_os_memory_free(d->buffer);
    solar_os_memory_free(d->shadow);
    epd_relax_dispose(&d->relax);
    if (d->pins_configured) {
        (void)gpio_reset_pin(d->dc_pin);
        (void)gpio_reset_pin(d->reset_pin);
        (void)gpio_reset_pin(d->busy_pin);
        if (d->power_pin >= 0) (void)gpio_reset_pin(d->power_pin);
    }
    if (d->lock) xSemaphoreGive(d->lock);
    memset(d, 0, sizeof(*d));
}

u8g2_t *epd_ssd1680_get_u8g2(epd_ssd1680_t *d) { return d ? &d->u8g2 : NULL; }

static esp_err_t relax_native(void *context, solar_os_display_relax_phase_t phase, const uint8_t *image)
{
    epd_ssd1680_t *d = context;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (d->powered && !d->suspended)
        err = phase == SOLAR_OS_DISPLAY_RELAX_BEGIN ? controller_init(d) : native_frame(d, image, false);
    return record_error(d, err);
}

esp_err_t epd_ssd1680_relax(epd_ssd1680_t *d, solar_os_display_relax_phase_t phase)
{
    if (!d || !d->buffer || (unsigned)phase > SOLAR_OS_DISPLAY_RELAX_END) return ESP_ERR_INVALID_ARG;
    if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
    esp_err_t err = epd_relax_apply(&d->relax, d->buffer, EPD_SSD1680_BUFFER_BYTES, phase, relax_native, d);
    d->last_error = err;
    if (d->lock) xSemaphoreGive(d->lock);
    return err;
}

const char *epd_ssd1680_controller_mode(const epd_ssd1680_t *d)
{
    if (!d) return NULL;
    if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
    const char *mode = d->refresh_mode == EPD_SSD1680_REFRESH_FULL ? "full" :
        d->refresh_mode == EPD_SSD1680_REFRESH_PARTIAL ? "partial" : "auto";
    char *description = ((epd_ssd1680_t *)d)->mode_description;
    snprintf(description, sizeof(d->mode_description),
             "refresh=%s waveform=otp panel=0 size=122x250 cleanup-every=%u", mode, PARTIAL_CLEANUP_EVERY);
    if (d->lock) xSemaphoreGive(d->lock);
    return description;
}

const char *epd_ssd1680_controller_mode_values(const epd_ssd1680_t *d)
{
    (void)d;
    return "refresh=<auto,fast,partial,full> waveform=otp";
}

esp_err_t epd_ssd1680_set_controller_mode(epd_ssd1680_t *d, const char *mode)
{
    if (!d || !mode) return ESP_ERR_INVALID_ARG;
    epd_ssd1680_refresh_mode_t next;
    if (!strcmp(mode, "refresh=auto") || !strcmp(mode, "waveform=otp")) next = EPD_SSD1680_REFRESH_AUTO;
    else if (!strcmp(mode, "refresh=full")) next = EPD_SSD1680_REFRESH_FULL;
    else if (!strcmp(mode, "refresh=partial") || !strcmp(mode, "refresh=fast")) next = EPD_SSD1680_REFRESH_PARTIAL;
    else return ESP_ERR_NOT_SUPPORTED;
    if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!d->relax.storage) {
        if (d->refresh_mode != next) d->shadow_valid = false;
        d->refresh_mode = next;
        err = ESP_OK;
    }
    if (d->lock) xSemaphoreGive(d->lock);
    return err;
}

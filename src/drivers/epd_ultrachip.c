#include "epd_ultrachip.h"

#include <stddef.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_buses.h"

#define BUSY_TIMEOUT_US 30000000LL
#define AUTO_FULL_INTERVAL 20U
#define TRY(call) do { esp_err_t result_ = (call); if (result_ != ESP_OK) return result_; } while (0)

static void delay_ms(unsigned ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

esp_err_t epd_ultrachip_command(epd_ultrachip_t *d, uint8_t command,
                               const uint8_t *data, size_t length)
{
    if (!d || !d->spi || length > EPD_ULTRACHIP_ROW_BYTES || (length && !data))
        return ESP_ERR_INVALID_ARG;
    TRY(gpio_set_level(d->dc_pin, 0));
    spi_transaction_t transaction = {
        .flags = SPI_TRANS_USE_TXDATA, .length = 8, .tx_data = {command},
    };
    TRY(spi_device_polling_transmit(d->spi, &transaction));
    if (!length) return ESP_OK;
    TRY(gpio_set_level(d->dc_pin, 1));
    memcpy(d->line_buffer, data, length);
    transaction = (spi_transaction_t){.length = length * 8U, .tx_buffer = d->line_buffer};
    return spi_device_polling_transmit(d->spi, &transaction);
}

esp_err_t epd_ultrachip_wait_ready(epd_ultrachip_t *d)
{
    const int64_t deadline = esp_timer_get_time() + BUSY_TIMEOUT_US;
    /* BUSY is active low. Allow delayed assertion after PON/DRF/POF. */
    delay_ms(100);
    while (gpio_get_level(d->busy_pin) == 0) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        delay_ms(20);
    }
    return ESP_OK;
}

static esp_err_t external_power(epd_ultrachip_t *d, bool on)
{
    if (d->power_pin >= 0) TRY(gpio_set_level(d->power_pin, on));
    d->powered = on;
    if (on) delay_ms(20);
    return ESP_OK;
}

esp_err_t epd_ultrachip_power_on(epd_ultrachip_t *d)
{
    if (d->analog_on) return ESP_OK;
    TRY(epd_ultrachip_command(d, 0x04, NULL, 0));
    TRY(epd_ultrachip_wait_ready(d));
    d->analog_on = true;
    return ESP_OK;
}

static esp_err_t controller_init(epd_ultrachip_t *d)
{
    d->controller_ready = d->shadow_valid = d->analog_on = false;
    d->partial_count = 0;
    TRY(external_power(d, true));
    TRY(gpio_set_level(d->reset_pin, 1)); delay_ms(50);
    TRY(gpio_set_level(d->reset_pin, 0)); delay_ms(10);
    TRY(gpio_set_level(d->reset_pin, 1)); delay_ms(50);
    TRY(epd_ultrachip_wait_ready(d));
    TRY(d->panel->initialize(d));
    d->controller_ready = true;
    return ESP_OK;
}

/* SolarOS stores vertical, LSB-first tiles with 1=white. Panel planes use horizontal
 * MSB-first pixels with 1=white. Keep CS asserted over all addressed gates. */
static esp_err_t write_plane(epd_ultrachip_t *d, uint8_t command,
                              const uint8_t *frame, bool invert)
{
    TRY(epd_ultrachip_command(d, command, NULL, 0));
    TRY(gpio_set_level(d->dc_pin, 1));
    TRY(spi_device_acquire_bus(d->spi, portMAX_DELAY));
    esp_err_t err = ESP_OK;
    for (unsigned gate = 0; gate < d->panel->gate_count && err == ESP_OK; ++gate) {
        memset(d->line_buffer, 0xff, EPD_ULTRACHIP_ROW_BYTES);
        if (frame && gate >= d->panel->gate_offset &&
            gate < d->panel->gate_offset + EPD_ULTRACHIP_HEIGHT) {
            unsigned y = gate - d->panel->gate_offset;
            if (d->panel->reverse_rows) y = EPD_ULTRACHIP_HEIGHT - 1U - y;
            const uint8_t *tile = frame + (y / 8U) * EPD_ULTRACHIP_WIDTH;
            memset(d->line_buffer, 0, EPD_ULTRACHIP_ROW_BYTES);
            for (unsigned x = 0; x < EPD_ULTRACHIP_WIDTH; ++x) {
                if ((tile[x] & (1U << (y % 8U))) != 0)
                    d->line_buffer[x / 8U] |= (uint8_t)(0x80U >> (x % 8U));
            }
            if (invert) {
                for (unsigned x = 0; x < EPD_ULTRACHIP_ROW_BYTES; ++x)
                    d->line_buffer[x] ^= 0xffU;
            }
        }
        spi_transaction_t transaction = {
            .flags = gate + 1U < d->panel->gate_count ? SPI_TRANS_CS_KEEP_ACTIVE : 0,
            .length = EPD_ULTRACHIP_ROW_BYTES * 8U, .tx_buffer = d->line_buffer,
        };
        err = spi_device_polling_transmit(d->spi, &transaction);
    }
    /* A failed transfer may leave CS held; terminate the burst before releasing
     * the bus. The next refresh resets the controller and rebuilds both planes. */
    if (err != ESP_OK) {
        spi_transaction_t end = {.length = 0};
        (void)spi_device_polling_transmit(d->spi, &end);
    }
    spi_device_release_bus(d->spi);
    return err;
}

static esp_err_t refresh_frame(epd_ultrachip_t *d, const uint8_t *source, bool force_full)
{
    if (!d->controller_ready) TRY(controller_init(d));
    const bool baseline = d->shadow && d->shadow_valid;
    if (!force_full && baseline && d->refresh_mode != EPD_ULTRACHIP_REFRESH_FULL &&
        memcmp(d->shadow, source, EPD_ULTRACHIP_BUFFER_BYTES) == 0) return ESP_OK;
    const bool partial = !force_full && baseline && d->refresh_mode != EPD_ULTRACHIP_REFRESH_FULL &&
        (d->refresh_mode == EPD_ULTRACHIP_REFRESH_PARTIAL ||
         d->partial_count + 1U < AUTO_FULL_INTERVAL);
    TRY(write_plane(d, 0x13, source, false));
    /* Full cleanup drives every visible pixel through a transition; a partial
     * uses the last successfully displayed frame, including erased ink. */
    TRY(write_plane(d, 0x10, partial ? d->shadow : source, !partial));
    TRY(d->panel->activate(d, partial));
    TRY(epd_ultrachip_wait_ready(d));
    if (partial) TRY(epd_ultrachip_command(d, 0x92, NULL, 0));
    if (d->panel->finish) TRY(d->panel->finish(d));
    TRY(write_plane(d, 0x10, source, false));
    TRY(epd_ultrachip_command(d, 0x02, NULL, 0));
    TRY(epd_ultrachip_wait_ready(d));
    d->analog_on = false;
    if (d->shadow) {
        memcpy(d->shadow, source, EPD_ULTRACHIP_BUFFER_BYTES);
        d->shadow_valid = true;
    }
    d->partial_count = partial ? d->partial_count + 1U : 0;
    return ESP_OK;
}

esp_err_t epd_ultrachip_refresh(epd_ultrachip_t *d)
{
    if (!d || !d->spi || !d->buffer || !d->line_buffer || !d->panel)
        return ESP_ERR_INVALID_STATE;
    if (d->relax.storage) return ESP_OK;
    esp_err_t err = refresh_frame(d, d->buffer, false);
    if (err != ESP_OK) d->controller_ready = d->shadow_valid = false;
    d->last_error = err;
    return err;
}

static esp_err_t sleep_panel(epd_ultrachip_t *d)
{
    esp_err_t err = ESP_OK;
    if (d->powered) {
        err = epd_ultrachip_command(d, 0x02, NULL, 0);
        if (err == ESP_OK) err = epd_ultrachip_wait_ready(d);
        if (err == ESP_OK) err = epd_ultrachip_command(d, 0x07, (uint8_t[]){0xa5}, 1);
    }
    const esp_err_t power_err = external_power(d, false);
    d->controller_ready = d->shadow_valid = d->analog_on = false;
    return err != ESP_OK ? err : power_err;
}

static const u8x8_display_info_t display_info = {
    .chip_enable_level = 0, .chip_disable_level = 1,
    .reset_pulse_width_ms = 10, .post_reset_wait_ms = 50,
    .sck_clock_hz = 16000000, .spi_mode = 0,
    .tile_width = EPD_ULTRACHIP_WIDTH / 8U,
    .tile_height = EPD_ULTRACHIP_HEIGHT / 8U,
    .pixel_width = EPD_ULTRACHIP_WIDTH, .pixel_height = EPD_ULTRACHIP_HEIGHT,
};

static uint8_t display_cb(u8x8_t *u8x8, uint8_t message, uint8_t arg, void *ptr)
{
    (void)ptr;
    if (message == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(u8x8, &display_info);
        return 1;
    }
    epd_ultrachip_t *d = (epd_ultrachip_t *)((uint8_t *)u8x8 -
        offsetof(epd_ultrachip_t, u8g2) - offsetof(u8g2_t, u8x8));
    esp_err_t err;
    switch (message) {
    case U8X8_MSG_DISPLAY_INIT: err = controller_init(d); break;
    case U8X8_MSG_DISPLAY_SET_POWER_SAVE:
        err = arg ? sleep_panel(d) : (d->controller_ready ? ESP_OK : controller_init(d));
        break;
    case U8X8_MSG_DISPLAY_REFRESH: err = epd_ultrachip_refresh(d); break;
    case U8X8_MSG_DISPLAY_DRAW_TILE: return 1;
    default: return 0;
    }
    d->last_error = err;
    return err == ESP_OK;
}

static bool config_valid(const epd_ultrachip_config_t *c)
{
    if (!c || !c->spi_bus || !c->spi_bus[0] || !c->rotation ||
        c->spi_clock_hz < 100000 || c->spi_clock_hz > 20000000 || c->power_pin < -1)
        return false;
    const int pins[] = {c->cs_pin, c->dc_pin, c->reset_pin, c->busy_pin, c->power_pin};
    for (unsigned i = 0; i < 5; ++i) {
        if (i == 4 && pins[i] == -1) continue;
        if (pins[i] < 0 || pins[i] >= GPIO_NUM_MAX || pins[i] >= 64) return false;
        for (unsigned j = i + 1; j < 5; ++j) if (pins[i] == pins[j]) return false;
    }
    return true;
}

static esp_err_t configure_pins(epd_ultrachip_t *d)
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

esp_err_t epd_ultrachip_init(epd_ultrachip_t *d, const epd_ultrachip_config_t *c,
                            const epd_ultrachip_panel_t *panel)
{
    if (!d || !config_valid(c) || !panel || !panel->initialize || !panel->activate ||
        panel->gate_offset + EPD_ULTRACHIP_HEIGHT > panel->gate_count)
        return ESP_ERR_INVALID_ARG;
    *d = (epd_ultrachip_t){.panel = panel, .dc_pin = c->dc_pin,
        .reset_pin = c->reset_pin, .busy_pin = c->busy_pin, .power_pin = c->power_pin};
    esp_err_t err = configure_pins(d);
    if (err == ESP_OK) err = external_power(d, false);
    if (err == ESP_OK) {
        const spi_device_interface_config_t device_config = {
            .clock_speed_hz = c->spi_clock_hz, .mode = 0,
            .spics_io_num = c->cs_pin, .queue_size = 1,
        };
        err = solar_os_bus_spi_add_device(c->spi_bus, &device_config, &d->spi);
    }
    if (err == ESP_OK) {
        d->line_buffer = heap_caps_malloc(EPD_ULTRACHIP_ROW_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        d->buffer = heap_caps_calloc(1, EPD_ULTRACHIP_BUFFER_BYTES, MALLOC_CAP_8BIT);
        d->shadow = heap_caps_malloc(EPD_ULTRACHIP_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!d->shadow) d->shadow = heap_caps_malloc(EPD_ULTRACHIP_BUFFER_BYTES, MALLOC_CAP_8BIT);
        if (!d->line_buffer || !d->buffer) err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        u8g2_SetupDisplay(&d->u8g2, display_cb, u8x8_dummy_cb, u8x8_dummy_cb, u8x8_dummy_cb);
        u8g2_SetupBuffer(&d->u8g2, d->buffer, EPD_ULTRACHIP_HEIGHT / 8U,
                         u8g2_ll_hvline_vertical_top_lsb, c->rotation);
        u8g2_InitDisplay(&d->u8g2);
        err = d->last_error;
    }
    if (err != ESP_OK) epd_ultrachip_deinit(d);
    return err;
}

esp_err_t epd_ultrachip_resume(epd_ultrachip_t *d)
{
    if (!d || !d->spi || !d->buffer) return ESP_ERR_INVALID_STATE;
    esp_err_t err = configure_pins(d);
    if (err == ESP_OK) err = controller_init(d);
    d->last_error = err;
    return err;
}

void epd_ultrachip_deinit(epd_ultrachip_t *d)
{
    if (!d) return;
    epd_relax_dispose(&d->relax);
    if (d->spi && d->powered) (void)sleep_panel(d);
    if (d->spi) (void)spi_bus_remove_device(d->spi);
    heap_caps_free(d->line_buffer); heap_caps_free(d->buffer); heap_caps_free(d->shadow);
    if (d->pins_configured) {
        (void)gpio_reset_pin(d->dc_pin); (void)gpio_reset_pin(d->reset_pin);
        (void)gpio_reset_pin(d->busy_pin);
        if (d->power_pin >= 0) (void)gpio_reset_pin(d->power_pin);
    }
    memset(d, 0, sizeof(*d));
}

u8g2_t *epd_ultrachip_get_u8g2(epd_ultrachip_t *d) { return d ? &d->u8g2 : NULL; }

const char *epd_ultrachip_controller_mode(const epd_ultrachip_t *d)
{
    if (!d) return NULL;
    if (d->refresh_mode == EPD_ULTRACHIP_REFRESH_PARTIAL) return "refresh=partial";
    if (d->refresh_mode == EPD_ULTRACHIP_REFRESH_FULL) return "refresh=full";
    return "refresh=auto";
}

const char *epd_ultrachip_controller_mode_values(const epd_ultrachip_t *d)
{
    (void)d;
    return "refresh=<auto,partial,full>";
}

esp_err_t epd_ultrachip_set_controller_mode(epd_ultrachip_t *d, const char *mode)
{
    if (!d || !mode) return ESP_ERR_INVALID_ARG;
    if (d->relax.storage) return ESP_ERR_INVALID_STATE;
    if (strcmp(mode, "refresh=auto") == 0) d->refresh_mode = EPD_ULTRACHIP_REFRESH_AUTO;
    else if (strcmp(mode, "refresh=partial") == 0) d->refresh_mode = EPD_ULTRACHIP_REFRESH_PARTIAL;
    else if (strcmp(mode, "refresh=full") == 0) d->refresh_mode = EPD_ULTRACHIP_REFRESH_FULL;
    else return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static esp_err_t relax_native(void *context, solar_os_display_relax_phase_t phase,
                               const uint8_t *image)
{
    epd_ultrachip_t *d = context;
    if (phase == SOLAR_OS_DISPLAY_RELAX_BEGIN) {
        d->controller_ready = d->shadow_valid = false;
        return ESP_OK;
    }
    const esp_err_t err = d->powered ? refresh_frame(d, image, true) : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) d->controller_ready = d->shadow_valid = false;
    return err;
}

esp_err_t epd_ultrachip_relax(epd_ultrachip_t *d, solar_os_display_relax_phase_t phase)
{
    if (!d || !d->buffer || (unsigned)phase > SOLAR_OS_DISPLAY_RELAX_END) return ESP_ERR_INVALID_ARG;
    if (phase == SOLAR_OS_DISPLAY_RELAX_BEGIN && !d->powered) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = epd_relax_apply(&d->relax, d->buffer, EPD_ULTRACHIP_BUFFER_BYTES,
                                         phase, relax_native, d);
    d->last_error = err;
    return err;
}

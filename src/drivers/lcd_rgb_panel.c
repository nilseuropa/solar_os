#include "lcd_rgb_panel.h"

#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "io_expander_ch422g.h"
#include "solar_os_board.h"
#include "solar_os_vector.h"

#ifndef SOLAR_OS_BOARD_DISPLAY_RGB_PCLK_ACTIVE_NEG
#define SOLAR_OS_BOARD_DISPLAY_RGB_PCLK_ACTIVE_NEG 0
#endif
#ifndef SOLAR_OS_BOARD_DISPLAY_RGB_DISP_ACTIVE_LEVEL
#define SOLAR_OS_BOARD_DISPLAY_RGB_DISP_ACTIVE_LEVEL 1
#endif

/* True electrical scan geometry -- these drive h_res/v_res and the
 * esp_lcd framebuffer's fixed memory stride, and must never change
 * without re-deriving HSYNC/VSYNC timing for the new axes. */
#define LCD_PHYS_WIDTH SOLAR_OS_BOARD_DISPLAY_WIDTH
#define LCD_PHYS_HEIGHT SOLAR_OS_BOARD_DISPLAY_HEIGHT

/*
 * The glass is wired/scanned correctly (h_res/v_res above are right --
 * a mismatch there would corrupt/tear the image, not just rotate it),
 * but is mounted in the case rotated 90 degrees from how the product
 * is meant to be viewed. There's no MADCTL-equivalent for a "dumb" RGB
 * panel to compensate in hardware, so u8g2 draws into a NATIVE buffer
 * that's the transpose of the physical one (rotation handled by u8g2's
 * own R1, presenting SOLAR_OS_BOARD_DISPLAY_WIDTH/HEIGHT to apps as
 * usual) and draw_tile below rotates each pixel into the physical,
 * fixed-stride framebuffer.
 *
 * LCD_ROTATE_CW selects the rotation direction; if the image comes out
 * upside-down-ish/mirrored instead of matching u8g2's rotation, flip
 * both this and U8G2_R1 (below, in lcd_rgb_panel_init) to U8G2_R3
 * together -- they must agree on direction.
 */
#define LCD_ROTATE_CW 1
#if LCD_ROTATE_CW
#define LCD_U8G2_ROTATION U8G2_R1
#else
#define LCD_U8G2_ROTATION U8G2_R3
#endif

#define LCD_NATIVE_WIDTH LCD_PHYS_HEIGHT
#define LCD_NATIVE_HEIGHT LCD_PHYS_WIDTH
#define LCD_NATIVE_TILE_WIDTH ((LCD_NATIVE_WIDTH + 7) / 8)
#define LCD_NATIVE_TILE_HEIGHT ((LCD_NATIVE_HEIGHT + 7) / 8)
#define LCD_NATIVE_BUFFER_ROW_BYTES (LCD_NATIVE_TILE_WIDTH * 8)
#define LCD_NATIVE_BUFFER_BYTES (LCD_NATIVE_BUFFER_ROW_BYTES * LCD_NATIVE_TILE_HEIGHT)
#define LCD_RGB565_BLACK 0x0000
#define LCD_RGB565_WHITE 0xffff

static const char *TAG = "lcd_rgb_panel";
static lcd_rgb_panel_t *active_display;
static SemaphoreHandle_t lcd_vsync_sem;

/*
 * Present path: the naive approach (rotate each u8g2 tile pixel by
 * pixel straight into the PSRAM frame buffer) scatters 16-bit writes
 * 1600 bytes apart, so almost every write costs a full cache-line
 * round trip to PSRAM -- several MB of bus traffic per frame,
 * starving the bounce-buffer refill ISR that feeds the panel (visible
 * as shimmer/torn lines while apps repaint). Instead, each PHYSICAL
 * row is assembled in a small internal-RAM staging buffer via a
 * byte -> 8-pixels lookup table and then written to PSRAM as one
 * contiguous 1600-byte burst.
 *
 * lcd_byte_lut expands one native framebuffer byte (8 vertical
 * pixels, LSB = top) to 8 RGB565 words; lcd_byte_lut_rev is the same
 * with the words in reverse order, for the CCW mounting direction.
 */
static uint16_t lcd_byte_lut[256][8];
static uint16_t lcd_byte_lut_rev[256][8];
static uint16_t lcd_row_staging[LCD_PHYS_WIDTH];

static void lcd_rgb_init_luts(void)
{
    for (unsigned value = 0; value < 256; value++) {
        for (unsigned bit = 0; bit < 8; bit++) {
            const uint16_t px = (value & (1U << bit)) != 0 ? LCD_RGB565_WHITE : LCD_RGB565_BLACK;
            lcd_byte_lut[value][bit] = px;
            lcd_byte_lut_rev[value][7 - bit] = px;
        }
    }
}

/*
 * Convert the native 1bpp u8g2 buffer into the back frame buffer,
 * rotated into physical orientation. One native COLUMN nx is one
 * physical ROW:
 *   CW:  py = NATIVE_W-1-nx, px = ny  ->  for row py, nx = NATIVE_W-1-py
 *   CCW: py = nx,            px = NATIVE_H-1-ny (reversed pixel order)
 *
 * Only rows whose native column changed since the previous present
 * are written: the shadow copy diffs the u8g2 buffer page by page,
 * and the union with dirty_prev covers what the back buffer (one
 * flip behind) is still missing. An unchanged frame costs only the
 * sequential compare -- no PSRAM frame-buffer writes, no flip.
 */
_Static_assert(LCD_NATIVE_TILE_WIDTH <= sizeof(((lcd_rgb_panel_t *)0)->dirty_prev),
               "dirty_prev bitmap too small for this panel width");

static void lcd_rgb_present_frame(lcd_rgb_panel_t *display)
{
    const uint8_t *buffer = display->buffer;
    uint16_t *fb = (uint16_t *)display->framebuffers[display->back_fb];
    uint8_t dirty_cur[LCD_NATIVE_TILE_WIDTH] = {0};
    bool changed = false;

    for (int page = 0; page < LCD_NATIVE_TILE_HEIGHT; page++) {
        const uint8_t *src = &buffer[(size_t)page * LCD_NATIVE_BUFFER_ROW_BYTES];
        uint8_t *shadow = &display->shadow[(size_t)page * LCD_NATIVE_BUFFER_ROW_BYTES];
        if (memcmp(src, shadow, LCD_NATIVE_BUFFER_ROW_BYTES) == 0) {
            continue;
        }
        for (int nx = 0; nx < LCD_NATIVE_WIDTH; nx++) {
            if (src[nx] != shadow[nx]) {
                dirty_cur[nx >> 3] |= (uint8_t)(1U << (nx & 7));
                changed = true;
            }
        }
        memcpy(shadow, src, LCD_NATIVE_BUFFER_ROW_BYTES);
    }

    if (!changed) {
        /* The front buffer already shows exactly this frame; whatever
         * the back buffer is missing stays recorded in dirty_prev. */
        return;
    }

    for (int py = 0; py < LCD_PHYS_HEIGHT; py++) {
#if LCD_ROTATE_CW
        const int nx = LCD_NATIVE_WIDTH - 1 - py;
#else
        const int nx = py;
#endif
        if (((dirty_cur[nx >> 3] | display->dirty_prev[nx >> 3]) &
             (1U << (nx & 7))) == 0) {
            continue;
        }
        for (int page = 0; page < LCD_NATIVE_TILE_HEIGHT; page++) {
            const uint8_t value = buffer[(size_t)page * LCD_NATIVE_BUFFER_ROW_BYTES + (size_t)nx];
#if LCD_ROTATE_CW
            memcpy(&lcd_row_staging[page * 8], lcd_byte_lut[value], 16);
#else
            memcpy(&lcd_row_staging[LCD_PHYS_WIDTH - 8 - (page * 8)],
                   lcd_byte_lut_rev[value], 16);
#endif
        }
        memcpy(&fb[(size_t)py * LCD_PHYS_WIDTH], lcd_row_staging,
               sizeof(lcd_row_staging));
    }

    /* Point the scan-out at the finished frame (takes effect at the
     * frame boundary) and wait one VSYNC before anything writes into
     * the buffer that was on screen until now. */
    (void)esp_lcd_panel_draw_bitmap((esp_lcd_panel_handle_t)display->panel,
                                    0, 0, LCD_PHYS_WIDTH, LCD_PHYS_HEIGHT,
                                    display->framebuffers[display->back_fb]);
    display->back_fb ^= 1U;
    memcpy(display->dirty_prev, dirty_cur, sizeof(dirty_cur));
    if (lcd_vsync_sem != NULL) {
        (void)xSemaphoreTake(lcd_vsync_sem, 0); /* drain stale give */
        (void)xSemaphoreTake(lcd_vsync_sem, pdMS_TO_TICKS(50));
    }
}

/* Runs in ISR context; must live in IRAM (CONFIG_LCD_RGB_ISR_IRAM_SAFE). */
static IRAM_ATTR bool lcd_rgb_on_vsync(esp_lcd_panel_handle_t panel,
                                       const esp_lcd_rgb_panel_event_data_t *edata,
                                       void *user_ctx)
{
    (void)panel;
    (void)edata;
    (void)user_ctx;

    BaseType_t woken = pdFALSE;
    if (lcd_vsync_sem != NULL) {
        (void)xSemaphoreGiveFromISR(lcd_vsync_sem, &woken);
    }
    return woken == pdTRUE;
}

static const u8x8_display_info_t lcd_rgb_display_info = {
    .chip_enable_level = 0,
    .chip_disable_level = 1,
    .post_chip_enable_wait_ns = 0,
    .pre_chip_disable_wait_ns = 0,
    .reset_pulse_width_ms = 10,
    .post_reset_wait_ms = 100,
    .sda_setup_time_ns = 0,
    .sck_pulse_width_ns = 0,
    .sck_clock_hz = 0,
    .spi_mode = 0,
    .i2c_bus_clock_100kHz = 0,
    .data_setup_time_ns = 0,
    .write_pulse_width_ns = 0,
    .tile_width = LCD_NATIVE_TILE_WIDTH,
    .tile_height = LCD_NATIVE_TILE_HEIGHT,
    .default_x_offset = 0,
    .flipmode_x_offset = 0,
    .pixel_width = LCD_NATIVE_WIDTH,
    .pixel_height = LCD_NATIVE_HEIGHT,
};

static void lcd_rgb_reset_panel(void)
{
    /* LCD_RST lives on the CH422G expander, not an ESP32 GPIO. */
    io_expander_ch422g_set_pin(SOLAR_OS_BOARD_CH422G_PIN_LCD_RST, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    io_expander_ch422g_set_pin(SOLAR_OS_BOARD_CH422G_PIN_LCD_RST, true);
    vTaskDelay(pdMS_TO_TICKS(100));
}

static esp_err_t lcd_rgb_apply_backlight(uint8_t percent)
{
    /* Backlight is a plain expander switch (AP3032KTR boost driver
     * enable), not a PWM/analog dimmer -- any nonzero percent is "on". */
    const bool active = SOLAR_OS_BOARD_DISPLAY_RGB_DISP_ACTIVE_LEVEL != 0;
    const bool level = (percent > 0) ? active : !active;
    return io_expander_ch422g_set_pin(SOLAR_OS_BOARD_CH422G_PIN_BACKLIGHT, level);
}

static uint8_t lcd_rgb_u8x8_display_cb(u8x8_t *u8x8,
                                       uint8_t message,
                                       uint8_t arg_int,
                                       void *arg_ptr)
{
    if (message == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(u8x8, &lcd_rgb_display_info);
        return 1;
    }

    lcd_rgb_panel_t *display = active_display;
    if (display == NULL) {
        return 0;
    }

    switch (message) {
    case U8X8_MSG_DISPLAY_INIT:
        return 1;

    case U8X8_MSG_DISPLAY_SET_POWER_SAVE:
        if (arg_int == 0) {
            display->backlight_power = true;
            return lcd_rgb_apply_backlight(display->backlight_percent) == ESP_OK ? 1 : 0;
        }
        display->backlight_power = false;
        return lcd_rgb_apply_backlight(0) == ESP_OK ? 1 : 0;

    case U8X8_MSG_DISPLAY_DRAW_TILE: {
        /*
         * Every present is a full u8g2_SendBuffer over the finished
         * native buffer (which we own -- display->buffer), so the
         * per-tile payload is redundant: the FIRST tile triggers one
         * optimized whole-frame conversion (see lcd_rgb_present_frame)
         * and every other tile of the same pass is a no-op.
         */
        const u8x8_tile_t *tile = (const u8x8_tile_t *)arg_ptr;
        if (tile == NULL ||
            display->framebuffers[display->back_fb] == NULL ||
            display->buffer == NULL || display->shadow == NULL) {
            return 0;
        }
        if (tile->x_pos == 0 && tile->y_pos == 0) {
            lcd_rgb_present_frame(display);
        }
        return 1;
    }

    default:
        return 0;
    }
}

static uint8_t lcd_rgb_u8x8_byte_cb(u8x8_t *u8x8, uint8_t message, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8;
    (void)message;
    (void)arg_int;
    (void)arg_ptr;
    return 1;
}

esp_err_t lcd_rgb_panel_init(lcd_rgb_panel_t *display)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(display, 0, sizeof(*display));
    display->last_error = ESP_OK;
    display->backlight_percent = 100;
    lcd_rgb_init_luts();

    ESP_RETURN_ON_ERROR(io_expander_ch422g_init(), TAG, "io expander init failed");
    ESP_RETURN_ON_ERROR(lcd_rgb_apply_backlight(0), TAG, "backlight off failed");
    lcd_rgb_reset_panel();

#ifdef SOLAR_OS_BOARD_CH422G_PIN_SD_CS
    /*
     * SD card CS also lives on the CH422G (SOLAR_OS_BOARD_PIN_SD_CARD_CS
     * is GPIO_NUM_NC), and sd_card.c has no hook to drive an
     * expander-based CS per-transaction. This is the one dedicated SPI
     * bus on this board (SD is the only device on it), so it's safe to
     * just assert CS low once, permanently, here -- display init always
     * runs before the SD card gets mounted.
     */
    ESP_RETURN_ON_ERROR(io_expander_ch422g_set_pin(SOLAR_OS_BOARD_CH422G_PIN_SD_CS, false),
                        TAG,
                        "sd card cs assert failed");
#endif

    const esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = SOLAR_OS_BOARD_DISPLAY_RGB_PCLK_HZ,
            .h_res = LCD_PHYS_WIDTH,
            .v_res = LCD_PHYS_HEIGHT,
            .hsync_pulse_width = SOLAR_OS_BOARD_DISPLAY_RGB_HSYNC_PULSE,
            .hsync_back_porch = SOLAR_OS_BOARD_DISPLAY_RGB_HSYNC_BACK_PORCH,
            .hsync_front_porch = SOLAR_OS_BOARD_DISPLAY_RGB_HSYNC_FRONT_PORCH,
            .vsync_pulse_width = SOLAR_OS_BOARD_DISPLAY_RGB_VSYNC_PULSE,
            .vsync_back_porch = SOLAR_OS_BOARD_DISPLAY_RGB_VSYNC_BACK_PORCH,
            .vsync_front_porch = SOLAR_OS_BOARD_DISPLAY_RGB_VSYNC_FRONT_PORCH,
            .flags = {
                .pclk_active_neg = SOLAR_OS_BOARD_DISPLAY_RGB_PCLK_ACTIVE_NEG,
            },
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 2,
        /* 20 lines of bounce slack: the refill ISR competes with big
         * PSRAM-heavy presents (full-screen apps); 10 lines was tight
         * enough to underrun visibly during them. */
        .bounce_buffer_size_px = LCD_PHYS_WIDTH * 20,
        .dma_burst_size = 64,
        .hsync_gpio_num = SOLAR_OS_BOARD_PIN_LCD_RGB_HSYNC,
        .vsync_gpio_num = SOLAR_OS_BOARD_PIN_LCD_RGB_VSYNC,
        .de_gpio_num = SOLAR_OS_BOARD_PIN_LCD_RGB_DE,
        .pclk_gpio_num = SOLAR_OS_BOARD_PIN_LCD_RGB_PCLK,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            SOLAR_OS_BOARD_PIN_LCD_RGB_DATA0,
            SOLAR_OS_BOARD_PIN_LCD_RGB_DATA1,
            SOLAR_OS_BOARD_PIN_LCD_RGB_DATA2,
            SOLAR_OS_BOARD_PIN_LCD_RGB_DATA3,
            SOLAR_OS_BOARD_PIN_LCD_RGB_DATA4,
            SOLAR_OS_BOARD_PIN_LCD_RGB_DATA5,
            SOLAR_OS_BOARD_PIN_LCD_RGB_DATA6,
            SOLAR_OS_BOARD_PIN_LCD_RGB_DATA7,
            SOLAR_OS_BOARD_PIN_LCD_RGB_DATA8,
            SOLAR_OS_BOARD_PIN_LCD_RGB_DATA9,
            SOLAR_OS_BOARD_PIN_LCD_RGB_DATA10,
            SOLAR_OS_BOARD_PIN_LCD_RGB_DATA11,
            SOLAR_OS_BOARD_PIN_LCD_RGB_DATA12,
            SOLAR_OS_BOARD_PIN_LCD_RGB_DATA13,
            SOLAR_OS_BOARD_PIN_LCD_RGB_DATA14,
            SOLAR_OS_BOARD_PIN_LCD_RGB_DATA15,
        },
        .flags = {
            .fb_in_psram = 1,
        },
    };

    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&panel_config, &panel), TAG, "new rgb panel failed");
    display->panel = panel;

    esp_err_t err = esp_lcd_panel_reset(panel);
    if (err != ESP_OK) {
        lcd_rgb_panel_deinit(display);
        return err;
    }
    err = esp_lcd_panel_init(panel);
    if (err != ESP_OK) {
        lcd_rgb_panel_deinit(display);
        return err;
    }

    void *fb0 = NULL;
    void *fb1 = NULL;
    err = esp_lcd_rgb_panel_get_frame_buffer(panel, 2, &fb0, &fb1);
    if (err != ESP_OK || fb0 == NULL || fb1 == NULL) {
        lcd_rgb_panel_deinit(display);
        return err == ESP_OK ? ESP_FAIL : err;
    }
    display->framebuffers[0] = fb0;
    display->framebuffers[1] = fb1;
    memset(fb0, 0, (size_t)LCD_PHYS_WIDTH * LCD_PHYS_HEIGHT * 2U);
    memset(fb1, 0, (size_t)LCD_PHYS_WIDTH * LCD_PHYS_HEIGHT * 2U);
    /* Scan-out starts on framebuffer 0; render into the other one. */
    display->back_fb = 1;

    if (lcd_vsync_sem == NULL) {
        lcd_vsync_sem = xSemaphoreCreateBinary();
    }
    const esp_lcd_rgb_panel_event_callbacks_t callbacks = {
        .on_vsync = lcd_rgb_on_vsync,
    };
    err = esp_lcd_rgb_panel_register_event_callbacks(panel, &callbacks, NULL);
    if (err != ESP_OK) {
        lcd_rgb_panel_deinit(display);
        return err;
    }

    display->buffer_size = LCD_NATIVE_BUFFER_BYTES;
    display->buffer = heap_caps_malloc(display->buffer_size, MALLOC_CAP_8BIT);
    if (display->buffer == NULL) {
        lcd_rgb_panel_deinit(display);
        return ESP_ERR_NO_MEM;
    }
    memset(display->buffer, 0, display->buffer_size);

    display->shadow = heap_caps_malloc(LCD_NATIVE_BUFFER_BYTES,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (display->shadow == NULL) {
        display->shadow = heap_caps_malloc(LCD_NATIVE_BUFFER_BYTES, MALLOC_CAP_8BIT);
    }
    if (display->shadow == NULL) {
        lcd_rgb_panel_deinit(display);
        return ESP_ERR_NO_MEM;
    }
    memset(display->shadow, 0, LCD_NATIVE_BUFFER_BYTES);
    memset(display->dirty_prev, 0, sizeof(display->dirty_prev));

    u8g2_SetupDisplay(&display->u8g2,
                      lcd_rgb_u8x8_display_cb,
                      u8x8_dummy_cb,
                      lcd_rgb_u8x8_byte_cb,
                      u8x8_dummy_cb);
    u8g2_SetupBuffer(&display->u8g2,
                     display->buffer,
                     LCD_NATIVE_TILE_HEIGHT,
                     u8g2_ll_hvline_vertical_top_lsb,
                     LCD_U8G2_ROTATION);
    active_display = display;
    u8g2_InitDisplay(&display->u8g2);
    u8g2_SetPowerSave(&display->u8g2, 0);

    return display->last_error;
}

esp_err_t lcd_rgb_panel_resume(lcd_rgb_panel_t *display)
{
    if (display == NULL || display->panel == NULL || display->buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    active_display = display;
    display->last_error = ESP_OK;
    u8g2_InitDisplay(&display->u8g2);
    u8g2_SetPowerSave(&display->u8g2, 0);
    return display->last_error;
}

void lcd_rgb_panel_deinit(lcd_rgb_panel_t *display)
{
    if (display == NULL) {
        return;
    }

    lcd_rgb_apply_backlight(0);

    if (display->panel != NULL) {
        esp_lcd_panel_del((esp_lcd_panel_handle_t)display->panel);
        display->panel = NULL;
    }
    if (display->buffer != NULL) {
        heap_caps_free(display->buffer);
        display->buffer = NULL;
    }
    if (display->shadow != NULL) {
        heap_caps_free(display->shadow);
        display->shadow = NULL;
    }

    display->framebuffers[0] = NULL;
    display->framebuffers[1] = NULL;
    display->back_fb = 0;

    if (active_display == display) {
        active_display = NULL;
    }

    display->buffer_size = 0;
}

u8g2_t *lcd_rgb_panel_get_u8g2(lcd_rgb_panel_t *display)
{
    return display == NULL ? NULL : &display->u8g2;
}

bool lcd_rgb_panel_backlight_supported(void)
{
    return true;
}

esp_err_t lcd_rgb_panel_get_backlight(const lcd_rgb_panel_t *display, uint8_t *percent)
{
    if (percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (display == NULL) {
        *percent = 0;
        return ESP_ERR_INVALID_STATE;
    }

    *percent = display->backlight_percent;
    return ESP_OK;
}

esp_err_t lcd_rgb_panel_set_backlight(lcd_rgb_panel_t *display, uint8_t percent)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    display->backlight_percent = percent;
    if (!display->backlight_power) {
        return ESP_OK;
    }

    return lcd_rgb_apply_backlight(percent);
}

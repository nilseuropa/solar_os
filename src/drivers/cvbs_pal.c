/*
 * SolarOS monochrome PAL scanout for the original ESP32 DAC.
 *
 * The 625-line timing, field sync table, APLL coefficients, and I2S/DAC
 * setup are adapted from LovyanGFX Panel_CVBS:
 * https://github.com/lovyan03/LovyanGFX
 *
 * LovyanGFX is distributed under the FreeBSD license. Copyright (c) lovyan03
 * and contributors. The original implementation also credits Roger Cheng's
 * ESP_8_BIT_composite and rossumur's esp_8_bit projects.
 */

#include "cvbs_pal.h"

#include <string.h>

#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_ipc.h"
#include "esp_log.h"
#include "esp_private/periph_ctrl.h"
#include "esp_rom_sys.h"
#include "hal/dac_ll.h"
#include "hal/dac_types.h"
#include "soc/i2s_struct.h"
#include "soc/periph_defs.h"
#include "soc/rtc.h"

#define CVBS_NATIVE_WIDTH CVBS_PAL_HEIGHT
#define CVBS_NATIVE_HEIGHT CVBS_PAL_WIDTH
#define CVBS_TILE_WIDTH ((CVBS_NATIVE_WIDTH + 7U) / 8U)
#define CVBS_TILE_HEIGHT ((CVBS_NATIVE_HEIGHT + 7U) / 8U)
#define CVBS_BUFFER_SIZE (CVBS_TILE_WIDTH * CVBS_TILE_HEIGHT * 8U)

#if SOLAR_OS_CVBS_MODE_320X200
/*
 * Conservative non-interlaced PAL timing for small displays. The 320x200
 * canvas is centered in the 312-line frame and uses one DAC sample per pixel.
 */
#define PAL_TOTAL_SCANLINES 312U
#define PAL_SCANLINE_SAMPLES 472U
#define PAL_SYNC_SAMPLES 35U
#define PAL_SHORT_SYNC_SAMPLES 17U
#define PAL_LONG_SYNC_SAMPLES 201U
#define PAL_VISIBLE_START 66U
#define PAL_VISIBLE_END (PAL_VISIBLE_START + CVBS_PAL_HEIGHT)
#define PAL_TRAILING_SYNC_START 309U
#define PAL_MONO_ACTIVE_START 108U
#define CVBS_LEVEL_SYNC 0U
#define CVBS_LEVEL_BLANKING 23U
#define CVBS_LEVEL_BLACK 23U
#define CVBS_LEVEL_WHITE 77U
#else
#define PAL_TOTAL_SCANLINES 625U
#define PAL_FIELD_START 312U
#define PAL_SCANLINE_SAMPLES 568U
#define PAL_VSYNC_LINES 25U
#define PAL_SYNC_SAMPLES 42U
#define PAL_EQUALIZING_SAMPLES 20U
#define PAL_LONG_SYNC_SAMPLES 242U
#define PAL_ACTIVE_START 108U
#define PAL_MONO_ACTIVE_START (PAL_ACTIVE_START + 24U)
/* LovyanGFX's direct ESP32 DAC levels at the default CVBS output strength. */
#define CVBS_LEVEL_SYNC 0U
#define CVBS_LEVEL_BLANKING 28U
#define CVBS_LEVEL_BLACK 28U
#define CVBS_LEVEL_WHITE 90U
#endif
#define CVBS_PIXEL_PAIR(first, second) \
    (((uint32_t)(first) << 24U) | ((uint32_t)(second) << 8U))
#define CVBS_INTERRUPT_CORE 1U
#define CVBS_PRESENT_CORE 0U
#if !SOLAR_OS_CVBS_MODE_320X200
#define CVBS_STATIC_SYNC_SAMPLES PAL_LONG_SYNC_SAMPLES
#define CVBS_STATIC_BLANK_SAMPLES PAL_SCANLINE_SAMPLES
#endif

static const char *TAG = "cvbs-pal";
static cvbs_pal_t *active_display;
static DRAM_ATTR uint32_t pixel_lut[16][2];
static bool pixel_lut_ready;

static const u8x8_display_info_t cvbs_display_info = {
    .chip_enable_level = 0,
    .chip_disable_level = 1,
    .sck_clock_hz = 17734476UL,
    .i2c_bus_clock_100kHz = 0,
    .tile_width = CVBS_TILE_WIDTH,
    .tile_height = CVBS_TILE_HEIGHT,
    .pixel_width = CVBS_NATIVE_WIDTH,
    .pixel_height = CVBS_NATIVE_HEIGHT,
};

static inline void IRAM_ATTR fill_samples(uint16_t *buffer,
                                          size_t start,
                                          size_t count,
                                          uint8_t level)
{
    memset(&buffer[start], level, count * sizeof(uint16_t));
}

static inline void IRAM_ATTR render_pixels(cvbs_pal_t *display,
                                           uint16_t *buffer,
                                           uint16_t y)
{
    const uint8_t *frame = display->scanout_buffers[display->current_buffer];
    const uint8_t *row = &frame[(size_t)y * (CVBS_PAL_WIDTH / 8U)];
    uint32_t *output = (uint32_t *)&buffer[PAL_MONO_ACTIVE_START];

#if SOLAR_OS_CVBS_MODE_320X200
    /* I2S LCD mode swaps adjacent 16-bit samples, so each LUT word stores a
     * pair in transmission order. Two nibble lookups render eight pixels. */
    for (size_t group = 0; group < CVBS_PAL_WIDTH / 8U; group++) {
        const uint8_t packed = row[group];
        const uint32_t *pixels = pixel_lut[packed >> 4U];
        output[0] = pixels[0];
        output[1] = pixels[1];
        pixels = pixel_lut[packed & 0x0FU];
        output[2] = pixels[0];
        output[3] = pixels[1];
        output += 4;
    }
#else
    /*
     * I2S LCD mode swaps adjacent 16-bit samples, so each LUT word stores a
     * pair in transmission order. One sample per pixel halves both the ISR
     * work and DMA memory without reducing the 384-pixel canvas.
     */
    for (size_t group = 0; group < CVBS_PAL_WIDTH / 8U; group++) {
        const uint8_t packed = row[group];
        const uint32_t *pixels = pixel_lut[packed >> 4U];
        output[0] = pixels[0];
        output[1] = pixels[1];
        pixels = pixel_lut[packed & 0x0FU];
        output[2] = pixels[0];
        output[3] = pixels[1];
        output += 4;
    }
#endif
}

static inline void IRAM_ATTR render_normal_line(cvbs_pal_t *display,
                                                uint16_t *buffer,
                                                int y)
{
    fill_samples(buffer, 0, PAL_SYNC_SAMPLES, CVBS_LEVEL_SYNC);
    fill_samples(buffer,
                 PAL_SYNC_SAMPLES,
                 PAL_MONO_ACTIVE_START - PAL_SYNC_SAMPLES,
                 CVBS_LEVEL_BLACK);
    if (y >= 0 && y < (int)CVBS_PAL_HEIGHT) {
        render_pixels(display, buffer, (uint16_t)y);
    } else {
        fill_samples(buffer,
                     PAL_MONO_ACTIVE_START,
                     CVBS_PAL_WIDTH,
                     CVBS_LEVEL_BLACK);
    }
    fill_samples(buffer,
                 PAL_MONO_ACTIVE_START + CVBS_PAL_WIDTH,
                 PAL_SCANLINE_SAMPLES - PAL_MONO_ACTIVE_START -
                     CVBS_PAL_WIDTH,
                 CVBS_LEVEL_BLACK);
}

#if SOLAR_OS_CVBS_MODE_320X200
static inline void IRAM_ATTR render_vsync_line(uint16_t *buffer,
                                               uint16_t field_line)
{
    const bool first_long = field_line == 0U || field_line == 1U ||
                            field_line == 2U;
    const bool second_long = field_line == 0U || field_line == 1U;
    const size_t half = PAL_SCANLINE_SAMPLES / 2U;
    const size_t first_width = first_long
                                   ? PAL_LONG_SYNC_SAMPLES
                                   : PAL_SHORT_SYNC_SAMPLES;
    const size_t second_width = second_long
                                    ? PAL_LONG_SYNC_SAMPLES
                                    : PAL_SHORT_SYNC_SAMPLES;

    fill_samples(buffer, 0, first_width, CVBS_LEVEL_SYNC);
    fill_samples(buffer,
                 first_width,
                 half - first_width,
                 CVBS_LEVEL_BLANKING);
    fill_samples(buffer, half, second_width, CVBS_LEVEL_SYNC);
    fill_samples(buffer,
                 half + second_width,
                 half - second_width,
                 CVBS_LEVEL_BLANKING);
}
#endif

#if !SOLAR_OS_CVBS_MODE_320X200
static inline bool full_pal_visible_scanline(uint16_t scanline, uint16_t *y)
{
    const uint16_t field_line = scanline >= PAL_FIELD_START
                                    ? (uint16_t)(scanline - PAL_FIELD_START)
                                    : scanline;
    if (field_line < PAL_VSYNC_LINES ||
        field_line >= PAL_VSYNC_LINES + CVBS_PAL_HEIGHT) {
        return false;
    }
    if (y != NULL) {
        *y = (uint16_t)(field_line - PAL_VSYNC_LINES);
    }
    return true;
}

static void full_pal_sync_widths(uint16_t scanline,
                                 size_t *first_width,
                                 size_t *second_width)
{
    const bool odd_field = scanline >= PAL_FIELD_START;
    const uint16_t field_line = odd_field
                                    ? (uint16_t)(scanline - PAL_FIELD_START)
                                    : scanline;
    *first_width = PAL_SYNC_SAMPLES;
    *second_width = 0U;
    if (!odd_field) {
        if (field_line == 0U) {
            *second_width = PAL_EQUALIZING_SAMPLES;
            return;
        }
        if (field_line <= 2U || (field_line >= 6U && field_line <= 7U)) {
            *first_width = PAL_EQUALIZING_SAMPLES;
            *second_width = PAL_EQUALIZING_SAMPLES;
            return;
        }
        if (field_line <= 4U) {
            *first_width = PAL_LONG_SYNC_SAMPLES;
            *second_width = PAL_LONG_SYNC_SAMPLES;
            return;
        }
        if (field_line == 5U) {
            *first_width = PAL_LONG_SYNC_SAMPLES;
            *second_width = PAL_EQUALIZING_SAMPLES;
        }
        return;
    }
    if (field_line >= 1U && field_line <= 2U) {
        *first_width = PAL_EQUALIZING_SAMPLES;
        *second_width = PAL_EQUALIZING_SAMPLES;
        return;
    }
    if (field_line == 3U) {
        *first_width = PAL_EQUALIZING_SAMPLES;
        *second_width = PAL_LONG_SYNC_SAMPLES;
        return;
    }
    if (field_line >= 4U && field_line <= 5U) {
        *first_width = PAL_LONG_SYNC_SAMPLES;
        *second_width = PAL_LONG_SYNC_SAMPLES;
        return;
    }
    if (field_line >= 6U && field_line <= 7U) {
        *first_width = PAL_EQUALIZING_SAMPLES;
        *second_width = PAL_EQUALIZING_SAMPLES;
        return;
    }
    if (field_line == 8U) {
        *first_width = PAL_EQUALIZING_SAMPLES;
    }
}

static void configure_dma_descriptor(lldesc_t *descriptor,
                                     uint8_t *buffer,
                                     size_t length,
                                     bool eof)
{
    memset(descriptor, 0, sizeof(*descriptor));
    descriptor->buf = buffer;
    descriptor->owner = 1;
    descriptor->eof = eof ? 1U : 0U;
    descriptor->length = length;
    descriptor->size = length;
}

static uint16_t full_pal_next_dynamic_scanline(uint16_t scanline)
{
    const uint8_t slot = (uint8_t)(scanline % CVBS_DMA_DYNAMIC_LINE_COUNT);
    for (uint16_t offset = 1U; offset <= PAL_TOTAL_SCANLINES; offset++) {
        uint16_t candidate = (uint16_t)(scanline + offset);
        if (candidate >= PAL_TOTAL_SCANLINES) {
            candidate -= PAL_TOTAL_SCANLINES;
        }
        if (candidate % CVBS_DMA_DYNAMIC_LINE_COUNT == slot &&
            full_pal_visible_scanline(candidate, NULL)) {
            return candidate;
        }
    }
    return UINT16_MAX;
}
#endif

static inline void IRAM_ATTR accept_pending_frame(cvbs_pal_t *display)
{
    portENTER_CRITICAL_ISR(&display->buffer_lock);
    if (display->pending_buffer >= 0 &&
        display->pending_buffer != display->current_buffer) {
        display->current_buffer = display->pending_buffer;
        display->pending_buffer = -1;
    }
    portEXIT_CRITICAL_ISR(&display->buffer_lock);
}

#if SOLAR_OS_CVBS_MODE_320X200
static void IRAM_ATTR render_scanline(cvbs_pal_t *display,
                                      uint16_t *buffer,
                                      uint16_t scanline)
{
    if (scanline == 0U) {
        accept_pending_frame(display);
    }

    if (scanline < 5U || scanline >= PAL_TRAILING_SYNC_START) {
        render_vsync_line(buffer, scanline < 5U ? scanline : 4U);
    } else if (scanline >= PAL_VISIBLE_START && scanline < PAL_VISIBLE_END) {
        render_normal_line(display,
                           buffer,
                           (int)(scanline - PAL_VISIBLE_START));
    } else {
        render_normal_line(display, buffer, -1);
    }
}
#endif

static void IRAM_ATTR cvbs_i2s_isr(void *arg)
{
    cvbs_pal_t *display = (cvbs_pal_t *)arg;
    const bool eof = I2S0.int_st.out_eof;
    I2S0.int_clr.val = I2S0.int_st.val;
    if (!eof) {
        return;
    }

    lldesc_t *descriptor = (lldesc_t *)I2S0.out_eof_des_addr;
    const uintptr_t descriptor_offset =
        (uintptr_t)descriptor - (uintptr_t)&display->dma_desc[0];
    if (descriptor_offset >= sizeof(display->dma_desc) ||
        descriptor_offset % sizeof(display->dma_desc[0]) != 0U) {
        return;
    }

    const uint16_t descriptor_index =
        (uint16_t)(descriptor_offset / sizeof(display->dma_desc[0]));
#if SOLAR_OS_CVBS_MODE_320X200
    uint8_t elapsed = (uint8_t)(descriptor_index + CVBS_DMA_DESCRIPTOR_COUNT -
                                display->last_eof_descriptor);
    elapsed %= CVBS_DMA_DESCRIPTOR_COUNT;
    if (elapsed == 0U) {
        elapsed = CVBS_DMA_DESCRIPTOR_COUNT;
    }

    uint16_t transmitted_scanline =
        (uint16_t)(display->last_eof_scanline + elapsed);
    if (transmitted_scanline >= PAL_TOTAL_SCANLINES) {
        transmitted_scanline -= PAL_TOTAL_SCANLINES;
    }
    uint16_t refill_scanline =
        (uint16_t)(transmitted_scanline + CVBS_DMA_DESCRIPTOR_COUNT);
    if (refill_scanline >= PAL_TOTAL_SCANLINES) {
        refill_scanline -= PAL_TOTAL_SCANLINES;
    }

    render_scanline(display, (uint16_t *)descriptor->buf, refill_scanline);
    display->last_eof_descriptor = (uint8_t)descriptor_index;
    display->last_eof_scanline = transmitted_scanline;
#else
    const uint16_t refill_scanline =
        display->dma_refill_scanline[descriptor_index];
    if (refill_scanline != UINT16_MAX) {
        if (refill_scanline == PAL_VSYNC_LINES) {
            accept_pending_frame(display);
        }
        uint16_t y = 0U;
        if (full_pal_visible_scanline(refill_scanline, &y)) {
            render_normal_line(display, (uint16_t *)descriptor->buf, (int)y);
        }
    }
#endif
}

typedef struct {
    cvbs_pal_t *display;
    esp_err_t result;
} cvbs_interrupt_alloc_context_t;

static void cvbs_alloc_interrupt_on_current_core(void *arg)
{
    cvbs_interrupt_alloc_context_t *context =
        (cvbs_interrupt_alloc_context_t *)arg;
    context->result = esp_intr_alloc(ETS_I2S0_INTR_SOURCE,
                                     ESP_INTR_FLAG_LEVEL3 | ESP_INTR_FLAG_IRAM,
                                     cvbs_i2s_isr,
                                     context->display,
                                     &context->display->interrupt);
}

static esp_err_t cvbs_alloc_interrupt(cvbs_pal_t *display)
{
    cvbs_interrupt_alloc_context_t context = {
        .display = display,
        .result = ESP_FAIL,
    };

    if (xPortGetCoreID() == CVBS_INTERRUPT_CORE) {
        cvbs_alloc_interrupt_on_current_core(&context);
        return context.result;
    }

    const esp_err_t ipc_err = esp_ipc_call_blocking(
        CVBS_INTERRUPT_CORE,
        cvbs_alloc_interrupt_on_current_core,
        &context);
    return ipc_err == ESP_OK ? context.result : ipc_err;
}

static inline uint8_t reverse_bits(uint8_t value)
{
    value = (uint8_t)(((value & 0xF0U) >> 4U) | ((value & 0x0FU) << 4U));
    value = (uint8_t)(((value & 0xCCU) >> 2U) | ((value & 0x33U) << 2U));
    return (uint8_t)(((value & 0xAAU) >> 1U) | ((value & 0x55U) << 1U));
}

static int8_t cvbs_acquire_copy_buffer(cvbs_pal_t *display)
{
    int8_t target = -1;
    portENTER_CRITICAL(&display->buffer_lock);
    if (display->copying_buffer < 0) {
        target = display->pending_buffer >= 0
                     ? display->pending_buffer
                     : (int8_t)(1 - display->current_buffer);
        display->pending_buffer = -1;
        display->copying_buffer = target;
    }
    portEXIT_CRITICAL(&display->buffer_lock);
    return target;
}

static void cvbs_commit_copy_buffer(cvbs_pal_t *display, int8_t target)
{
    portENTER_CRITICAL(&display->buffer_lock);
    if (display->copying_buffer == target) {
        display->copying_buffer = -1;
        display->pending_buffer = target;
    }
    portEXIT_CRITICAL(&display->buffer_lock);
}

static esp_err_t cvbs_present_on_current_core(cvbs_pal_t *display)
{
    const int8_t target = cvbs_acquire_copy_buffer(display);
    if (target < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *scanout = display->scanout_buffers[target];
    for (size_t group = 0; group < CVBS_PAL_WIDTH / 8U; group++) {
        const uint8_t *source =
            &display->draw_buffer[group * CVBS_NATIVE_WIDTH];
        for (size_t y = 0; y < CVBS_PAL_HEIGHT; y++) {
            scanout[y * (CVBS_PAL_WIDTH / 8U) + group] =
                reverse_bits(source[CVBS_NATIVE_WIDTH - 1U - y]);
        }
        /* Leave a short SRAM/PSRAM bus window for I2S DMA between contiguous
         * transpose passes. The complete present remains below one PAL field. */
        esp_rom_delay_us(4U);
    }

    cvbs_commit_copy_buffer(display, target);
    return ESP_OK;
}

typedef struct {
    cvbs_pal_t *display;
    esp_err_t result;
} cvbs_present_context_t;

static void cvbs_present_on_cpu0(void *arg)
{
    cvbs_present_context_t *context = (cvbs_present_context_t *)arg;
    context->result = cvbs_present_on_current_core(context->display);
}

static esp_err_t cvbs_present(cvbs_pal_t *display)
{
    if (xPortGetCoreID() == CVBS_PRESENT_CORE) {
        return cvbs_present_on_current_core(display);
    }

    cvbs_present_context_t context = {
        .display = display,
        .result = ESP_FAIL,
    };
    const esp_err_t ipc_err = esp_ipc_call_blocking(CVBS_PRESENT_CORE,
                                                    cvbs_present_on_cpu0,
                                                    &context);
    return ipc_err == ESP_OK ? context.result : ipc_err;
}

esp_err_t cvbs_pal_present_mono_xbm(cvbs_pal_t *display,
                                    const uint8_t *bitmap,
                                    size_t bitmap_size,
                                    uint16_t x,
                                    uint16_t y,
                                    uint16_t width,
                                    uint16_t height,
                                    uint16_t stride,
                                    bool palette_inverted)
{
    const size_t source_bytes = (size_t)width / 8U;
    const size_t scanout_stride = CVBS_PAL_WIDTH / 8U;
    if (display == NULL || bitmap == NULL || width == 0 || height == 0 ||
        (x & 7U) != 0U || (width & 7U) != 0U || stride < source_bytes ||
        height > SIZE_MAX / stride || bitmap_size < (size_t)height * stride) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((uint32_t)x + width > CVBS_PAL_WIDTH ||
        (uint32_t)y + height > CVBS_PAL_HEIGHT) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const int8_t target = cvbs_acquire_copy_buffer(display);
    if (target < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *scanout = display->scanout_buffers[target];
    memset(scanout, palette_inverted ? 0x00 : 0xFF, display->buffer_size);
    const size_t destination_x = x / 8U;
    for (size_t row = 0; row < height; row++) {
        const uint8_t *source = bitmap + row * stride;
        uint8_t *destination =
            scanout + ((size_t)y + row) * scanout_stride + destination_x;
        for (size_t column = 0; column < source_bytes; column++) {
            /* XBM is LSB-first and uses one bits for black; scanout is
             * MSB-first and uses one bits for white. Palette inversion swaps
             * both the frame background and the bitmap pixels. */
            const uint8_t pixels = reverse_bits(source[column]);
            destination[column] = palette_inverted ? pixels : (uint8_t)~pixels;
        }
    }

    cvbs_commit_copy_buffer(display, target);
    return ESP_OK;
}

static void cvbs_stop_signal(cvbs_pal_t *display)
{
    if (display == NULL || !display->signal_started) {
        return;
    }

    display->signal_started = false;
    if (display->interrupt != NULL) {
        (void)esp_intr_disable(display->interrupt);
    }
    for (size_t i = 0; i < CVBS_DMA_DESCRIPTOR_COUNT; i++) {
        display->dma_desc[i].empty = 0;
    }
    if (display->interrupt != NULL) {
        (void)esp_intr_free(display->interrupt);
        display->interrupt = NULL;
    }

    I2S0.out_link.stop = 1;
    I2S0.out_link.start = 0;
    I2S0.conf.tx_start = 0;
    dac_ll_digi_enable_dma(false);
    dac_ll_power_down(DAC_CHAN_0);
    periph_module_disable(PERIPH_I2S0_MODULE);
    rtc_clk_apll_enable(false);

    heap_caps_free(display->dma_buffer);
    display->dma_buffer = NULL;
    display->dma_buffer_size = 0;
    heap_caps_free(display->dma_static_buffer);
    display->dma_static_buffer = NULL;
    display->dma_static_buffer_size = 0;
}

static esp_err_t cvbs_start_signal(cvbs_pal_t *display)
{
    if (display == NULL || display->scanout_buffers[0] == NULL ||
        display->scanout_buffers[1] == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (display->signal_started) {
        return ESP_OK;
    }
    if (display->config.output_pin != GPIO_NUM_25) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const size_t line_bytes = PAL_SCANLINE_SAMPLES * sizeof(uint16_t);
    display->dma_buffer_size = line_bytes * CVBS_DMA_DYNAMIC_LINE_COUNT;
    display->dma_buffer = heap_caps_calloc(1,
                                           display->dma_buffer_size,
                                           MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (display->dma_buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }
#if !SOLAR_OS_CVBS_MODE_320X200
    const size_t sync_bytes =
        CVBS_STATIC_SYNC_SAMPLES * sizeof(uint16_t);
    const size_t blank_bytes =
        CVBS_STATIC_BLANK_SAMPLES * sizeof(uint16_t);
    display->dma_static_buffer_size = sync_bytes + blank_bytes;
    display->dma_static_buffer = heap_caps_calloc(
        1,
        display->dma_static_buffer_size,
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (display->dma_static_buffer == NULL) {
        heap_caps_free(display->dma_buffer);
        display->dma_buffer = NULL;
        display->dma_buffer_size = 0;
        return ESP_ERR_NO_MEM;
    }
#endif
    /* From here on, cvbs_stop_signal() owns all partial-start cleanup. */
    display->signal_started = true;

#if SOLAR_OS_CVBS_MODE_320X200
    for (size_t i = 0; i < CVBS_DMA_DESCRIPTOR_COUNT; i++) {
        lldesc_t *descriptor = &display->dma_desc[i];
        memset(descriptor, 0, sizeof(*descriptor));
        descriptor->buf = &display->dma_buffer[i * line_bytes];
        descriptor->owner = 1;
        descriptor->eof = 1;
        descriptor->length = line_bytes;
        descriptor->size = line_bytes;
        descriptor->empty =
            (uint32_t)&display->dma_desc[(i + 1U) %
                                         CVBS_DMA_DESCRIPTOR_COUNT];
    }

    /* Seed the complete queue before the DMA engine starts. */
    for (size_t i = 0; i < CVBS_DMA_DESCRIPTOR_COUNT; i++) {
        render_scanline(display,
                        (uint16_t *)display->dma_desc[i].buf,
                        (uint16_t)i);
    }
    display->last_eof_descriptor = CVBS_DMA_DESCRIPTOR_COUNT - 1U;
    display->last_eof_scanline = PAL_TOTAL_SCANLINES - 1U;
#else
    uint8_t *sync_buffer = display->dma_static_buffer;
    uint8_t *blank_buffer = &display->dma_static_buffer[sync_bytes];
    memset(sync_buffer, CVBS_LEVEL_SYNC, sync_bytes);
    memset(blank_buffer, CVBS_LEVEL_BLANKING, blank_bytes);

    bool dynamic_seeded[CVBS_DMA_DYNAMIC_LINE_COUNT] = {false};
    uint16_t descriptor_index = 0U;
    for (uint16_t scanline = 0U; scanline < PAL_TOTAL_SCANLINES; scanline++) {
        uint16_t y = 0U;
        if (full_pal_visible_scanline(scanline, &y)) {
            const uint8_t slot =
                (uint8_t)(scanline % CVBS_DMA_DYNAMIC_LINE_COUNT);
            configure_dma_descriptor(
                &display->dma_desc[descriptor_index],
                &display->dma_buffer[slot * line_bytes],
                line_bytes,
                true);
            display->dma_refill_scanline[descriptor_index] =
                full_pal_next_dynamic_scanline(scanline);
            if (!dynamic_seeded[slot]) {
                render_normal_line(
                    display,
                    (uint16_t *)display->dma_desc[descriptor_index].buf,
                    y);
                dynamic_seeded[slot] = true;
            }
            descriptor_index++;
        } else {
            size_t first_width = 0U;
            size_t second_width = 0U;
            full_pal_sync_widths(scanline, &first_width, &second_width);
            configure_dma_descriptor(
                &display->dma_desc[descriptor_index],
                sync_buffer,
                first_width * sizeof(uint16_t),
                false);
            display->dma_refill_scanline[descriptor_index++] = UINT16_MAX;
            if (second_width > 0U) {
                configure_dma_descriptor(
                    &display->dma_desc[descriptor_index],
                    blank_buffer,
                    (PAL_SCANLINE_SAMPLES / 2U - first_width) *
                        sizeof(uint16_t),
                    false);
                display->dma_refill_scanline[descriptor_index++] = UINT16_MAX;
                configure_dma_descriptor(
                    &display->dma_desc[descriptor_index],
                    sync_buffer,
                    second_width * sizeof(uint16_t),
                    false);
                display->dma_refill_scanline[descriptor_index++] = UINT16_MAX;
                configure_dma_descriptor(
                    &display->dma_desc[descriptor_index],
                    blank_buffer,
                    (PAL_SCANLINE_SAMPLES / 2U - second_width) *
                        sizeof(uint16_t),
                    true);
                display->dma_refill_scanline[descriptor_index++] = UINT16_MAX;
            } else {
                configure_dma_descriptor(
                    &display->dma_desc[descriptor_index],
                    blank_buffer,
                    (PAL_SCANLINE_SAMPLES - first_width) *
                        sizeof(uint16_t),
                    true);
                display->dma_refill_scanline[descriptor_index++] = UINT16_MAX;
            }
        }
    }
    if (descriptor_index != CVBS_DMA_DESCRIPTOR_COUNT) {
        ESP_LOGE(TAG,
                 "PAL DMA schedule has %u descriptors, expected %u",
                 descriptor_index,
                 CVBS_DMA_DESCRIPTOR_COUNT);
        cvbs_stop_signal(display);
        return ESP_ERR_INVALID_STATE;
    }
    for (uint16_t i = 0U; i < CVBS_DMA_DESCRIPTOR_COUNT; i++) {
        display->dma_desc[i].empty = (uint32_t)&display->dma_desc[
            (i + 1U) % CVBS_DMA_DESCRIPTOR_COUNT];
    }
#endif

    esp_err_t err = rtc_gpio_init(GPIO_NUM_25);
    if (err != ESP_OK) {
        cvbs_stop_signal(display);
        return err;
    }
    err = rtc_gpio_set_direction(GPIO_NUM_25, RTC_GPIO_MODE_DISABLED);
    if (err != ESP_OK) {
        cvbs_stop_signal(display);
        return err;
    }
    (void)rtc_gpio_pullup_dis(GPIO_NUM_25);
    (void)rtc_gpio_pulldown_dis(GPIO_NUM_25);
    dac_ll_power_on(DAC_CHAN_0);
    dac_ll_rtc_sync_by_adc(false);
    dac_ll_digi_enable_dma(true);

    periph_module_enable(PERIPH_I2S0_MODULE);
    err = cvbs_alloc_interrupt(display);
    if (err != ESP_OK) {
        cvbs_stop_signal(display);
        return err;
    }

    rtc_clk_apll_enable(true);
#if SOLAR_OS_CVBS_MODE_320X200
    rtc_clk_apll_coeff_set(6, 0xCD, 0xCC, 0x07);
#else
    rtc_clk_apll_coeff_set(1, 0x04, 0xA4, 0x06);
#endif

#if SOLAR_OS_CVBS_MODE_320X200
    I2S0.conf.val = 1;
    I2S0.conf.val = 0;
#else
    I2S0.conf.tx_reset = 1;
    I2S0.conf.tx_reset = 0;
#endif
    I2S0.conf.tx_right_first = 1;
    I2S0.conf.tx_mono = 1;
    I2S0.conf.tx_msb_shift = 0;
    I2S0.conf.tx_short_sync = 0;
    I2S0.conf2.lcd_en = 1;
    I2S0.conf_chan.tx_chan_mod = 1;
    I2S0.sample_rate_conf.tx_bits_mod = 16;
    I2S0.sample_rate_conf.tx_bck_div_num =
#if SOLAR_OS_CVBS_MODE_320X200
        1;
#else
        2;
#endif
    I2S0.clkm_conf.clka_en = 1;
    I2S0.clkm_conf.clkm_div_num = 1;
    I2S0.clkm_conf.clkm_div_b = 0;
    I2S0.clkm_conf.clkm_div_a = 1;
    I2S0.fifo_conf.tx_fifo_mod = 1;
    I2S0.fifo_conf.tx_fifo_mod_force_en = 1;
    I2S0.out_link.addr = (uint32_t)display->dma_desc;
#if SOLAR_OS_CVBS_MODE_320X200
    I2S0.conf.tx_start = 1;
#endif
    I2S0.out_link.start = 1;
    I2S0.int_clr.val = UINT32_MAX;
    I2S0.int_ena.out_eof = 1;

    err = esp_intr_enable(display->interrupt);
    if (err != ESP_OK) {
        cvbs_stop_signal(display);
        return err;
    }
#if !SOLAR_OS_CVBS_MODE_320X200
    I2S0.conf.tx_start = 1;
#endif
#if SOLAR_OS_CVBS_MODE_320X200
    ESP_LOGI(TAG,
             "PAL 312p/50 safe-area output on GPIO25, %ux%u, ISR CPU%d",
             CVBS_PAL_WIDTH,
             CVBS_PAL_HEIGHT,
             esp_intr_get_cpu(display->interrupt));
#else
    ESP_LOGI(TAG,
             "PAL 625/50 monochrome output on GPIO25, %ux%u, ISR CPU%d",
             CVBS_PAL_WIDTH,
             CVBS_PAL_HEIGHT,
             esp_intr_get_cpu(display->interrupt));
#endif
    return ESP_OK;
}

static uint8_t cvbs_u8x8_display_cb(u8x8_t *u8x8,
                                    uint8_t message,
                                    uint8_t arg_int,
                                    void *arg_ptr)
{
    (void)arg_ptr;
    if (message == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(u8x8, &cvbs_display_info);
        return 1;
    }

    cvbs_pal_t *display = active_display;
    if (display == NULL) {
        return 0;
    }

    esp_err_t err = ESP_OK;
    switch (message) {
    case U8X8_MSG_DISPLAY_INIT:
        err = cvbs_start_signal(display);
        break;
    case U8X8_MSG_DISPLAY_SET_POWER_SAVE:
        if (arg_int != 0U) {
            cvbs_stop_signal(display);
        } else {
            err = cvbs_start_signal(display);
        }
        break;
    case U8X8_MSG_DISPLAY_DRAW_TILE:
        return 1;
    case U8X8_MSG_DISPLAY_REFRESH:
        err = cvbs_present(display);
        break;
    default:
        return 0;
    }

    display->last_error = err;
    return err == ESP_OK ? 1 : 0;
}

esp_err_t cvbs_pal_init(cvbs_pal_t *display, const cvbs_pal_config_t *config)
{
    if (display == NULL || config == NULL || config->output_pin != GPIO_NUM_25) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(display, 0, sizeof(*display));
    display->config = *config;
    if (display->config.rotation == NULL) {
        display->config.rotation = U8G2_R1;
    }
    if (!pixel_lut_ready) {
        for (size_t pattern = 0; pattern < 16U; pattern++) {
            for (size_t pair = 0; pair < 2U; pair++) {
                const size_t first_bit = pair * 2U;
                const size_t second_bit = first_bit + 1U;
                const uint8_t first =
                    (pattern & (0x08U >> first_bit)) != 0U
                        ? CVBS_LEVEL_WHITE
                        : CVBS_LEVEL_BLACK;
                const uint8_t second =
                    (pattern & (0x08U >> second_bit)) != 0U
                        ? CVBS_LEVEL_WHITE
                        : CVBS_LEVEL_BLACK;
                pixel_lut[pattern][pair] = CVBS_PIXEL_PAIR(first, second);
            }
        }
        pixel_lut_ready = true;
    }
    display->buffer_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    display->current_buffer = 0;
    display->pending_buffer = -1;
    display->copying_buffer = -1;
    display->buffer_size = CVBS_BUFFER_SIZE;

    display->draw_buffer = heap_caps_calloc(1,
                                             display->buffer_size,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for (size_t i = 0; i < 2; i++) {
        display->scanout_buffers[i] = heap_caps_calloc(1,
                                                       display->buffer_size,
                                                       MALLOC_CAP_INTERNAL |
                                                           MALLOC_CAP_8BIT);
    }
    if (display->draw_buffer == NULL || display->scanout_buffers[0] == NULL ||
        display->scanout_buffers[1] == NULL) {
        cvbs_pal_deinit(display);
        return ESP_ERR_NO_MEM;
    }

    u8g2_SetupDisplay(&display->u8g2,
                      cvbs_u8x8_display_cb,
                      u8x8_cad_empty,
                      u8x8_dummy_cb,
                      u8x8_dummy_cb);
    u8g2_SetupBuffer(&display->u8g2,
                     display->draw_buffer,
                     CVBS_TILE_HEIGHT,
                     u8g2_ll_hvline_vertical_top_lsb,
                     display->config.rotation);
    active_display = display;
    u8g2_InitDisplay(&display->u8g2);
    if (display->last_error != ESP_OK) {
        const esp_err_t err = display->last_error;
        cvbs_pal_deinit(display);
        return err;
    }
    u8g2_ClearBuffer(&display->u8g2);
    return ESP_OK;
}

esp_err_t cvbs_pal_resume(cvbs_pal_t *display)
{
    if (display == NULL || display->draw_buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    active_display = display;
    display->last_error = cvbs_start_signal(display);
    return display->last_error;
}

void cvbs_pal_deinit(cvbs_pal_t *display)
{
    if (display == NULL) {
        return;
    }
    cvbs_stop_signal(display);
    if (active_display == display) {
        active_display = NULL;
    }
    heap_caps_free(display->draw_buffer);
    display->draw_buffer = NULL;
    for (size_t i = 0; i < 2; i++) {
        heap_caps_free(display->scanout_buffers[i]);
        display->scanout_buffers[i] = NULL;
    }
}

u8g2_t *cvbs_pal_get_u8g2(cvbs_pal_t *display)
{
    return display != NULL ? &display->u8g2 : NULL;
}

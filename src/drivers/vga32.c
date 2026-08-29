/*
 * SolarOS VGA scanout for the LilyGO/TTGO VGA32 v1.4 resistor DAC.
 *
 * I2S1 continuously streams a short scanline ring. An IRAM interrupt on CPU1
 * refills completed line groups, so VGA consumes only a few KiB of DMA-capable
 * line memory and does not create a scheduler-visible task.
 */

#include "vga32.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_ipc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_private/periph_ctrl.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "soc/gpio_sig_map.h"
#include "soc/i2s_reg.h"
#include "soc/i2s_struct.h"
#include "soc/periph_defs.h"
#include "soc/rtc.h"

#define VGA32_NATIVE_WIDTH VGA32_HEIGHT
#define VGA32_NATIVE_HEIGHT VGA32_WIDTH
#define VGA32_TILE_WIDTH ((VGA32_NATIVE_WIDTH + 7U) / 8U)
#define VGA32_TILE_HEIGHT ((VGA32_NATIVE_HEIGHT + 7U) / 8U)
#define VGA32_DRAW_BUFFER_SIZE \
    (VGA32_TILE_WIDTH * VGA32_TILE_HEIGHT * 8U)
#define VGA32_SCANOUT_STRIDE (VGA32_WIDTH / 8U)
#define VGA32_SCANOUT_BUFFER_SIZE (VGA32_SCANOUT_STRIDE * VGA32_HEIGHT)

#if (SOLAR_OS_VGA_MODE_320X200 + SOLAR_OS_VGA_MODE_320X240 + \
     SOLAR_OS_VGA_MODE_640X400 + SOLAR_OS_VGA_MODE_640X480) != 1
#error "Exactly one SolarOS VGA mode must be selected"
#endif

/* FabGL-compatible 31.5 kHz VGA modelines. */
#if SOLAR_OS_VGA_MODE_320X200
#define VGA32_H_VISIBLE 320U
#define VGA32_H_FRONT_PORCH 8U
#define VGA32_H_SYNC 48U
#define VGA32_H_BACK_PORCH 24U
#define VGA32_V_VISIBLE 200U
#define VGA32_V_FRONT_PORCH 6U
#define VGA32_V_SYNC 1U
#define VGA32_V_BACK_PORCH 17U
#define VGA32_SCAN_COUNT 2U
#define VGA32_REFRESH_HZ 70U
#define VGA32_PIXEL_CLOCK_HZ 12587500UL
#define VGA32_APLL_O_DIV 5U
#define VGA32_APLL_SDM0 174U
#define VGA32_APLL_SDM1 207U
#define VGA32_APLL_SDM2 4U
#define VGA32_DMA_INTERRUPT_LINES 2U
#elif SOLAR_OS_VGA_MODE_320X240
#define VGA32_H_VISIBLE 320U
#define VGA32_H_FRONT_PORCH 8U
#define VGA32_H_SYNC 48U
#define VGA32_H_BACK_PORCH 24U
#define VGA32_V_VISIBLE 240U
#define VGA32_V_FRONT_PORCH 5U
#define VGA32_V_SYNC 1U
#define VGA32_V_BACK_PORCH 16U
#define VGA32_SCAN_COUNT 2U
#define VGA32_REFRESH_HZ 60U
#define VGA32_PIXEL_CLOCK_HZ 12587500UL
#define VGA32_APLL_O_DIV 5U
#define VGA32_APLL_SDM0 174U
#define VGA32_APLL_SDM1 207U
#define VGA32_APLL_SDM2 4U
#define VGA32_DMA_INTERRUPT_LINES 2U
#elif SOLAR_OS_VGA_MODE_640X400
#define VGA32_H_VISIBLE 640U
#define VGA32_H_FRONT_PORCH 16U
#define VGA32_H_SYNC 96U
#define VGA32_H_BACK_PORCH 48U
#define VGA32_V_VISIBLE 400U
#define VGA32_V_FRONT_PORCH 12U
#define VGA32_V_SYNC 2U
#define VGA32_V_BACK_PORCH 35U
#define VGA32_SCAN_COUNT 1U
#define VGA32_REFRESH_HZ 70U
#define VGA32_PIXEL_CLOCK_HZ 25175000UL
#define VGA32_APLL_O_DIV 2U
#define VGA32_APLL_SDM0 235U
#define VGA32_APLL_SDM1 17U
#define VGA32_APLL_SDM2 6U
#define VGA32_DMA_INTERRUPT_LINES 4U
#else
#define VGA32_H_VISIBLE 640U
#define VGA32_H_FRONT_PORCH 16U
#define VGA32_H_SYNC 96U
#define VGA32_H_BACK_PORCH 48U
#define VGA32_V_VISIBLE 480U
#define VGA32_V_FRONT_PORCH 10U
#define VGA32_V_SYNC 2U
#define VGA32_V_BACK_PORCH 33U
#define VGA32_SCAN_COUNT 1U
#define VGA32_REFRESH_HZ 60U
#define VGA32_PIXEL_CLOCK_HZ 25175000UL
#define VGA32_APLL_O_DIV 2U
#define VGA32_APLL_SDM0 235U
#define VGA32_APLL_SDM1 17U
#define VGA32_APLL_SDM2 6U
#define VGA32_DMA_INTERRUPT_LINES 4U
#endif

#define VGA32_H_TOTAL \
    (VGA32_H_VISIBLE + VGA32_H_FRONT_PORCH + VGA32_H_SYNC + \
     VGA32_H_BACK_PORCH)
#define VGA32_H_SYNC_START (VGA32_H_VISIBLE + VGA32_H_FRONT_PORCH)
#define VGA32_H_SYNC_END (VGA32_H_SYNC_START + VGA32_H_SYNC)

#define VGA32_V_TOTAL \
    (VGA32_V_VISIBLE + VGA32_V_FRONT_PORCH + VGA32_V_SYNC + \
     VGA32_V_BACK_PORCH)
#define VGA32_PHYSICAL_LINE_COUNT (VGA32_V_TOTAL * VGA32_SCAN_COUNT)
#define VGA32_V_SYNC_START (VGA32_V_VISIBLE + VGA32_V_FRONT_PORCH)
#define VGA32_V_SYNC_END (VGA32_V_SYNC_START + VGA32_V_SYNC)

#define VGA32_RED0_BIT 0U
#define VGA32_RED1_BIT 1U
#define VGA32_GREEN0_BIT 2U
#define VGA32_GREEN1_BIT 3U
#define VGA32_BLUE0_BIT 4U
#define VGA32_BLUE1_BIT 5U
#define VGA32_HSYNC_BIT 6U
#define VGA32_VSYNC_BIT 7U
#define VGA32_COLOR_MASK 0x3FU
#define VGA32_HSYNC_IDLE (1U << VGA32_HSYNC_BIT)
#define VGA32_VSYNC_IDLE (1U << VGA32_VSYNC_BIT)
#define VGA32_SYNC_IDLE (VGA32_HSYNC_IDLE | VGA32_VSYNC_IDLE)

#define VGA32_INTERRUPT_CORE 1U
#define VGA32_PRESENT_CORE 1U
#define VGA32_PRESENT_FRAME_INTERVAL_MS 33U
#define VGA32_PRESENT_TASK_PRIORITY 2U
#define VGA32_PRESENT_TASK_STACK_SIZE 4096U

static const char *TAG = "vga32";
static vga32_t *active_display;

static const u8x8_display_info_t vga32_display_info = {
    .chip_enable_level = 0,
    .chip_disable_level = 1,
    .sck_clock_hz = VGA32_PIXEL_CLOCK_HZ,
    .i2c_bus_clock_100kHz = 0,
    .tile_width = VGA32_TILE_WIDTH,
    .tile_height = VGA32_TILE_HEIGHT,
    .pixel_width = VGA32_NATIVE_WIDTH,
    .pixel_height = VGA32_NATIVE_HEIGHT,
};

static void vga32_rebuild_pixel_lut(vga32_t *display)
{
    for (size_t packed = 0; packed < 256U; packed++) {
        uint8_t pixel[8];
        for (size_t bit = 0; bit < 8U; bit++) {
            const uint8_t color = (packed & (1U << bit)) != 0U
                                      ? display->background
                                      : display->foreground;
            pixel[bit] = VGA32_SYNC_IDLE | color;
        }
        /* I2S1 transmits bytes 2,3,0,1 within each 32-bit word. */
        display->pixel_lut[packed][0] =
            (uint32_t)pixel[2] | ((uint32_t)pixel[3] << 8U) |
            ((uint32_t)pixel[0] << 16U) | ((uint32_t)pixel[1] << 24U);
        display->pixel_lut[packed][1] =
            (uint32_t)pixel[6] | ((uint32_t)pixel[7] << 8U) |
            ((uint32_t)pixel[4] << 16U) | ((uint32_t)pixel[5] << 24U);
    }
}

static void IRAM_ATTR vga32_render_scanline(vga32_t *display,
                                            uint8_t *line,
                                            uint16_t physical_line)
{
    const uint16_t logical_line = physical_line / VGA32_SCAN_COUNT;
    if (physical_line == 0U) {
        portENTER_CRITICAL_ISR(&display->buffer_lock);
        if (display->pending_buffer >= 0 &&
            display->pending_buffer != display->current_buffer) {
            display->current_buffer = display->pending_buffer;
            display->pending_buffer = -1;
        }
        portEXIT_CRITICAL_ISR(&display->buffer_lock);
    }

    const bool vsync_asserted = logical_line >= VGA32_V_SYNC_START &&
                                logical_line < VGA32_V_SYNC_END;
    const uint8_t vertical = vsync_asserted ? 0U : VGA32_VSYNC_IDLE;
    const uint32_t blank = (uint32_t)(vertical | VGA32_HSYNC_IDLE) *
                           0x01010101U;
    const uint32_t sync = (uint32_t)vertical * 0x01010101U;
    uint32_t *words = (uint32_t *)line;
    for (size_t word = 0; word < VGA32_H_TOTAL / 4U; word++) {
        words[word] = blank;
    }
    for (size_t word = VGA32_H_SYNC_START / 4U;
         word < VGA32_H_SYNC_END / 4U;
         word++) {
        words[word] = sync;
    }

    if (logical_line >= VGA32_V_VISIBLE) {
        return;
    }

    const uint8_t *row =
        &display->scanout_buffers[display->current_buffer][
            (size_t)logical_line * VGA32_SCANOUT_STRIDE];
    for (size_t group = 0; group < VGA32_SCANOUT_STRIDE; group++) {
        const uint8_t packed = row[group];
        words[group * 2U] = display->pixel_lut[packed][0];
        words[group * 2U + 1U] = display->pixel_lut[packed][1];
    }
}

static void IRAM_ATTR vga32_i2s_isr(void *arg)
{
    vga32_t *display = (vga32_t *)arg;
    const bool eof = I2S1.int_st.out_eof;
    I2S1.int_clr.val = I2S1.int_st.val;
    if (!eof) {
        return;
    }

    lldesc_t *descriptor = (lldesc_t *)I2S1.out_eof_des_addr;
    const uintptr_t descriptor_offset =
        (uintptr_t)descriptor - (uintptr_t)&display->dma_desc[0];
    if (descriptor_offset >= sizeof(display->dma_desc) ||
        descriptor_offset % sizeof(display->dma_desc[0]) != 0U) {
        return;
    }

    const uint8_t descriptor_index =
        (uint8_t)(descriptor_offset / sizeof(display->dma_desc[0]));
    uint8_t elapsed =
        (uint8_t)(descriptor_index + VGA32_DMA_DESCRIPTOR_COUNT -
                  display->last_eof_descriptor);
    elapsed %= VGA32_DMA_DESCRIPTOR_COUNT;
    if (elapsed == 0U) {
        elapsed = VGA32_DMA_DESCRIPTOR_COUNT;
    }

    for (uint8_t step = 1U; step <= elapsed; step++) {
        const uint8_t slot =
            (uint8_t)((display->last_eof_descriptor + step) %
                      VGA32_DMA_DESCRIPTOR_COUNT);
        uint16_t transmitted =
            (uint16_t)(display->last_eof_scanline + step);
        if (transmitted >= VGA32_PHYSICAL_LINE_COUNT) {
            transmitted -= VGA32_PHYSICAL_LINE_COUNT;
        }
        uint16_t refill =
            (uint16_t)(transmitted + VGA32_DMA_DESCRIPTOR_COUNT);
        if (refill >= VGA32_PHYSICAL_LINE_COUNT) {
            refill -= VGA32_PHYSICAL_LINE_COUNT;
        }

        uint8_t *buffer = &display->dma_buffer[slot * VGA32_H_TOTAL];
#if VGA32_SCAN_COUNT == 2U
        if ((refill & 1U) != 0U) {
            const uint8_t previous =
                (uint8_t)((slot + VGA32_DMA_DESCRIPTOR_COUNT - 1U) %
                          VGA32_DMA_DESCRIPTOR_COUNT);
            memcpy(buffer,
                   &display->dma_buffer[previous * VGA32_H_TOTAL],
                   VGA32_H_TOTAL);
        } else {
            vga32_render_scanline(display, buffer, refill);
        }
#else
        vga32_render_scanline(display, buffer, refill);
#endif
    }

    uint16_t transmitted =
        (uint16_t)(display->last_eof_scanline + elapsed);
    while (transmitted >= VGA32_PHYSICAL_LINE_COUNT) {
        transmitted -= VGA32_PHYSICAL_LINE_COUNT;
    }
    display->last_eof_descriptor = descriptor_index;
    display->last_eof_scanline = transmitted;
}

typedef struct {
    vga32_t *display;
    esp_err_t result;
} vga32_interrupt_alloc_context_t;

static void vga32_alloc_interrupt_on_current_core(void *arg)
{
    vga32_interrupt_alloc_context_t *context =
        (vga32_interrupt_alloc_context_t *)arg;
    context->result = esp_intr_alloc(ETS_I2S1_INTR_SOURCE,
                                     ESP_INTR_FLAG_LEVEL3 | ESP_INTR_FLAG_IRAM,
                                     vga32_i2s_isr,
                                     context->display,
                                     &context->display->interrupt);
}

static esp_err_t vga32_alloc_interrupt(vga32_t *display)
{
    vga32_interrupt_alloc_context_t context = {
        .display = display,
        .result = ESP_FAIL,
    };
    if (xPortGetCoreID() == VGA32_INTERRUPT_CORE) {
        vga32_alloc_interrupt_on_current_core(&context);
        return context.result;
    }
    const esp_err_t ipc_err = esp_ipc_call_blocking(
        VGA32_INTERRUPT_CORE,
        vga32_alloc_interrupt_on_current_core,
        &context);
    return ipc_err == ESP_OK ? context.result : ipc_err;
}

static uint8_t vga32_rgb222(uint32_t rgb888)
{
    const uint8_t red = (uint8_t)((rgb888 >> 22U) & 0x03U);
    const uint8_t green = (uint8_t)((rgb888 >> 14U) & 0x03U);
    const uint8_t blue = (uint8_t)((rgb888 >> 6U) & 0x03U);
    return (uint8_t)(red | (green << VGA32_GREEN0_BIT) |
                     (blue << VGA32_BLUE0_BIT));
}

static esp_err_t vga32_route_pin(gpio_num_t pin, unsigned data_bit)
{
    esp_err_t err = gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    if (err != ESP_OK) {
        return err;
    }
    (void)gpio_pullup_dis(pin);
    (void)gpio_pulldown_dis(pin);
    (void)gpio_set_drive_capability(pin, GPIO_DRIVE_CAP_3);
    esp_rom_gpio_connect_out_signal((uint32_t)pin,
                                    I2S1O_DATA_OUT0_IDX + data_bit,
                                    false,
                                    false);
    return ESP_OK;
}

static esp_err_t vga32_route_pins(const vga32_t *display)
{
    const struct {
        gpio_num_t pin;
        unsigned bit;
    } routes[] = {
        {display->config.red0_pin, VGA32_RED0_BIT},
        {display->config.red1_pin, VGA32_RED1_BIT},
        {display->config.green0_pin, VGA32_GREEN0_BIT},
        {display->config.green1_pin, VGA32_GREEN1_BIT},
        {display->config.blue0_pin, VGA32_BLUE0_BIT},
        {display->config.blue1_pin, VGA32_BLUE1_BIT},
        {display->config.hsync_pin, VGA32_HSYNC_BIT},
        {display->config.vsync_pin, VGA32_VSYNC_BIT},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        const esp_err_t err = vga32_route_pin(routes[i].pin, routes[i].bit);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static void vga32_unroute_pins(const vga32_t *display)
{
    const gpio_num_t color_pins[] = {
        display->config.red0_pin,
        display->config.red1_pin,
        display->config.green0_pin,
        display->config.green1_pin,
        display->config.blue0_pin,
        display->config.blue1_pin,
    };
    for (size_t i = 0; i < sizeof(color_pins) / sizeof(color_pins[0]); i++) {
        esp_rom_gpio_connect_out_signal((uint32_t)color_pins[i],
                                        SIG_GPIO_OUT_IDX,
                                        false,
                                        false);
        (void)gpio_set_level(color_pins[i], 0);
    }
    const gpio_num_t sync_pins[] = {
        display->config.hsync_pin,
        display->config.vsync_pin,
    };
    for (size_t i = 0; i < sizeof(sync_pins) / sizeof(sync_pins[0]); i++) {
        esp_rom_gpio_connect_out_signal((uint32_t)sync_pins[i],
                                        SIG_GPIO_OUT_IDX,
                                        false,
                                        false);
        (void)gpio_set_level(sync_pins[i], 1);
    }
}

static void vga32_stop_signal(vga32_t *display)
{
    if (display == NULL || !display->signal_started) {
        return;
    }
    display->signal_started = false;
    if (display->interrupt != NULL) {
        (void)esp_intr_disable(display->interrupt);
    }
    for (size_t i = 0; i < VGA32_DMA_DESCRIPTOR_COUNT; i++) {
        display->dma_desc[i].empty = 0;
    }
    I2S1.out_link.stop = 1;
    I2S1.out_link.start = 0;
    I2S1.conf.tx_start = 0;
    if (display->interrupt != NULL) {
        (void)esp_intr_free(display->interrupt);
        display->interrupt = NULL;
    }
    periph_module_disable(PERIPH_I2S1_MODULE);
    rtc_clk_apll_enable(false);
    vga32_unroute_pins(display);
    heap_caps_free(display->dma_buffer);
    display->dma_buffer = NULL;
    display->dma_buffer_size = 0;
}

static esp_err_t vga32_start_signal(vga32_t *display)
{
    if (display == NULL || display->scanout_buffers[0] == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (display->signal_started) {
        return ESP_OK;
    }

    display->dma_buffer_size = VGA32_H_TOTAL * VGA32_DMA_DESCRIPTOR_COUNT;
    display->dma_buffer = heap_caps_calloc(
        1,
        display->dma_buffer_size,
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (display->dma_buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    display->signal_started = true;

    for (size_t i = 0; i < VGA32_DMA_DESCRIPTOR_COUNT; i++) {
        lldesc_t *descriptor = &display->dma_desc[i];
        memset(descriptor, 0, sizeof(*descriptor));
        descriptor->buf = &display->dma_buffer[i * VGA32_H_TOTAL];
        descriptor->owner = 1;
        descriptor->eof =
            i % VGA32_DMA_INTERRUPT_LINES ==
                    VGA32_DMA_INTERRUPT_LINES - 1U
                ? 1U
                : 0U;
        descriptor->length = VGA32_H_TOTAL;
        descriptor->size = VGA32_H_TOTAL;
        descriptor->empty = (uint32_t)&display->dma_desc[
            (i + 1U) % VGA32_DMA_DESCRIPTOR_COUNT];
#if VGA32_SCAN_COUNT == 2U
        if ((i & 1U) == 0U) {
            vga32_render_scanline(
                display,
                &display->dma_buffer[i * VGA32_H_TOTAL],
                (uint16_t)i);
        } else {
            memcpy(&display->dma_buffer[i * VGA32_H_TOTAL],
                   &display->dma_buffer[(i - 1U) * VGA32_H_TOTAL],
                   VGA32_H_TOTAL);
        }
#else
        vga32_render_scanline(display,
                              &display->dma_buffer[i * VGA32_H_TOTAL],
                              (uint16_t)i);
#endif
    }
    display->last_eof_descriptor = VGA32_DMA_DESCRIPTOR_COUNT - 1U;
    display->last_eof_scanline = VGA32_PHYSICAL_LINE_COUNT - 1U;

    esp_err_t err = vga32_route_pins(display);
    if (err != ESP_OK) {
        vga32_stop_signal(display);
        return err;
    }

    periph_module_reset(PERIPH_I2S1_MODULE);
    periph_module_enable(PERIPH_I2S1_MODULE);
    err = vga32_alloc_interrupt(display);
    if (err != ESP_OK) {
        vga32_stop_signal(display);
        return err;
    }

    I2S1.conf.tx_reset = 1;
    I2S1.conf.tx_reset = 0;
    I2S1.lc_conf.out_rst = 1;
    I2S1.lc_conf.out_rst = 0;
    I2S1.conf.tx_fifo_reset = 1;
    I2S1.conf.tx_fifo_reset = 0;

    I2S1.conf2.val = 0;
    I2S1.conf2.lcd_en = 1;
    I2S1.conf2.lcd_tx_wrx2_en = 1;
    I2S1.sample_rate_conf.val = 0;
    I2S1.sample_rate_conf.tx_bits_mod = 8;
    I2S1.sample_rate_conf.tx_bck_div_num = 1;

    rtc_clk_apll_enable(true);
    rtc_clk_apll_coeff_set(VGA32_APLL_O_DIV,
                           VGA32_APLL_SDM0,
                           VGA32_APLL_SDM1,
                           VGA32_APLL_SDM2);
    I2S1.clkm_conf.val = 0;
    I2S1.clkm_conf.clkm_div_num = 2;
    I2S1.clkm_conf.clkm_div_a = 1;
    I2S1.clkm_conf.clka_en = 1;

    I2S1.fifo_conf.val = 0;
    I2S1.fifo_conf.tx_fifo_mod_force_en = 1;
    I2S1.fifo_conf.tx_fifo_mod = 1;
    I2S1.fifo_conf.tx_data_num = 32;
    I2S1.fifo_conf.dscr_en = 1;
    I2S1.conf1.val = 0;
    I2S1.conf1.tx_pcm_bypass = 1;
    I2S1.conf_chan.val = 0;
    I2S1.conf_chan.tx_chan_mod = 1;
    I2S1.conf.tx_right_first = 1;
    I2S1.timing.val = 0;

    I2S1.lc_conf.ahbm_rst = 1;
    I2S1.lc_conf.ahbm_fifo_rst = 1;
    I2S1.lc_conf.ahbm_rst = 0;
    I2S1.lc_conf.ahbm_fifo_rst = 0;
    I2S1.lc_conf.val = I2S_OUT_DATA_BURST_EN | I2S_OUTDSCR_BURST_EN;
    I2S1.out_link.addr = (uint32_t)display->dma_desc;
    I2S1.int_clr.val = UINT32_MAX;
    I2S1.int_ena.out_eof = 1;
    I2S1.out_link.start = 1;
    I2S1.conf.tx_start = 1;
    err = esp_intr_enable(display->interrupt);
    if (err != ESP_OK) {
        vga32_stop_signal(display);
        return err;
    }

    ESP_LOGI(TAG,
             "VGA output ready: %ux%u@%uHz, I2S1 DMA ISR CPU%d",
             VGA32_WIDTH,
             VGA32_HEIGHT,
             VGA32_REFRESH_HZ,
             esp_intr_get_cpu(display->interrupt));
    return ESP_OK;
}

static int8_t vga32_acquire_copy_buffer(vga32_t *display)
{
    int8_t target = -1;
    portENTER_CRITICAL(&display->buffer_lock);
    if (display->copying_buffer < 0) {
#if VGA32_SCANOUT_BUFFER_COUNT == 1U
        target = display->current_buffer;
#else
        target = display->pending_buffer >= 0
                     ? display->pending_buffer
                     : (int8_t)(1 - display->current_buffer);
        display->pending_buffer = -1;
#endif
        display->copying_buffer = target;
    }
    portEXIT_CRITICAL(&display->buffer_lock);
    return target;
}

static void vga32_commit_copy_buffer(vga32_t *display, int8_t target)
{
    portENTER_CRITICAL(&display->buffer_lock);
    if (display->copying_buffer == target) {
        display->copying_buffer = -1;
#if VGA32_SCANOUT_BUFFER_COUNT > 1U
        display->pending_buffer = target;
#endif
    }
    portEXIT_CRITICAL(&display->buffer_lock);
}

static esp_err_t vga32_present_on_current_core(vga32_t *display,
                                               const uint8_t *source_buffer)
{
    if (source_buffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const int8_t target = vga32_acquire_copy_buffer(display);
    if (target < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t *scanout = display->scanout_buffers[target];
    for (size_t group = 0; group < VGA32_SCANOUT_STRIDE; group++) {
        const uint8_t *source =
            &source_buffer[group * VGA32_NATIVE_WIDTH];
        for (size_t y = 0; y < VGA32_HEIGHT; y++) {
            scanout[y * VGA32_SCANOUT_STRIDE + group] =
                source[VGA32_NATIVE_WIDTH - 1U - y];
        }
        esp_rom_delay_us(2U);
    }
    vga32_commit_copy_buffer(display, target);
    return ESP_OK;
}

static int8_t vga32_take_pending_present(vga32_t *display)
{
    int8_t source = -1;
    portENTER_CRITICAL(&display->present_lock);
    if (!display->present_stop_requested &&
        display->pending_present_buffer >= 0) {
        source = display->pending_present_buffer;
        display->pending_present_buffer = -1;
        display->rendering_present_buffer = source;
    }
    portEXIT_CRITICAL(&display->present_lock);
    return source;
}

static void vga32_present_task(void *arg)
{
    vga32_t *display = (vga32_t *)arg;
    TickType_t last_frame_tick = 0;

    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (true) {
            portENTER_CRITICAL(&display->present_lock);
            const bool stop = display->present_stop_requested;
            portEXIT_CRITICAL(&display->present_lock);
            if (stop) {
                goto stopped;
            }

            const int8_t source = vga32_take_pending_present(display);
            if (source < 0) {
                break;
            }

            const TickType_t now = xTaskGetTickCount();
            const TickType_t interval = pdMS_TO_TICKS(VGA32_PRESENT_FRAME_INTERVAL_MS);
            if (last_frame_tick != 0 && now - last_frame_tick < interval) {
                vTaskDelay(interval - (now - last_frame_tick));
            }

            const int64_t started_us = esp_timer_get_time();
            const esp_err_t err = vga32_present_on_current_core(
                display,
                display->present_buffers[source]);
            const uint32_t render_us =
                (uint32_t)(esp_timer_get_time() - started_us);
            last_frame_tick = xTaskGetTickCount();

            portENTER_CRITICAL(&display->present_lock);
            display->rendering_present_buffer = -1;
            if (err == ESP_OK) {
                display->present_rendered_frames++;
            }
            if (render_us > display->present_max_render_us) {
                display->present_max_render_us = render_us;
            }
            const bool more = display->pending_present_buffer >= 0;
            portEXIT_CRITICAL(&display->present_lock);

            if (err != ESP_OK) {
                ESP_LOGW(TAG, "asynchronous present failed: %s",
                         esp_err_to_name(err));
            }
            if (!more) {
                break;
            }
        }
    }

stopped:
    portENTER_CRITICAL(&display->present_lock);
    display->present_task = NULL;
    display->rendering_present_buffer = -1;
    portEXIT_CRITICAL(&display->present_lock);
    vTaskDelete(NULL);
}

static esp_err_t vga32_start_present_task(vga32_t *display)
{
    for (size_t i = 0; i < VGA32_PRESENT_BUFFER_COUNT; i++) {
        if (display->present_buffers[i] == NULL) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    const BaseType_t created = xTaskCreatePinnedToCore(
        vga32_present_task,
        "vga32-present",
        VGA32_PRESENT_TASK_STACK_SIZE,
        display,
        VGA32_PRESENT_TASK_PRIORITY,
        &display->present_task,
        VGA32_PRESENT_CORE);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "asynchronous present ready: CPU%u, max %u fps",
             (unsigned)VGA32_PRESENT_CORE,
             (unsigned)(1000U / VGA32_PRESENT_FRAME_INTERVAL_MS));
    return ESP_OK;
}

static void vga32_stop_present_task(vga32_t *display)
{
    TaskHandle_t task = NULL;
    portENTER_CRITICAL(&display->present_lock);
    display->present_stop_requested = true;
    task = display->present_task;
    portEXIT_CRITICAL(&display->present_lock);
    if (task != NULL) {
        xTaskNotifyGive(task);
        for (unsigned attempt = 0; attempt < 100U; attempt++) {
            portENTER_CRITICAL(&display->present_lock);
            const bool stopped = display->present_task == NULL;
            portEXIT_CRITICAL(&display->present_lock);
            if (stopped) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1U));
        }
    }
}

static esp_err_t vga32_present(vga32_t *display)
{
    if (display == NULL || display->draw_buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (display->present_task == NULL) {
        return vga32_present_on_current_core(display, display->draw_buffer);
    }

    int8_t target = -1;
    bool coalesced = false;
    portENTER_CRITICAL(&display->present_lock);
    if (!display->present_stop_requested &&
        display->copying_present_buffer < 0) {
        if (display->pending_present_buffer >= 0) {
            target = display->pending_present_buffer;
            display->pending_present_buffer = -1;
            coalesced = true;
        } else {
            target = display->rendering_present_buffer == 0 ? 1 : 0;
        }
        display->copying_present_buffer = target;
    }
    portEXIT_CRITICAL(&display->present_lock);
    if (target < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    const int64_t started_us = esp_timer_get_time();
    memcpy(display->present_buffers[target],
           display->draw_buffer,
           display->draw_buffer_size);
    const uint32_t copy_us = (uint32_t)(esp_timer_get_time() - started_us);

    TaskHandle_t task = NULL;
    portENTER_CRITICAL(&display->present_lock);
    display->copying_present_buffer = -1;
    display->pending_present_buffer = target;
    display->present_queued_frames++;
    if (coalesced) {
        display->present_coalesced_frames++;
    }
    if (copy_us > display->present_max_copy_us) {
        display->present_max_copy_us = copy_us;
    }
    task = display->present_task;
    portEXIT_CRITICAL(&display->present_lock);
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
    return ESP_OK;
}

static uint8_t vga32_u8x8_display_cb(u8x8_t *u8x8,
                                     uint8_t message,
                                     uint8_t arg_int,
                                     void *arg_ptr)
{
    (void)arg_ptr;
    if (message == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(u8x8, &vga32_display_info);
        return 1;
    }
    vga32_t *display = active_display;
    if (display == NULL) {
        return 0;
    }

    esp_err_t err = ESP_OK;
    switch (message) {
    case U8X8_MSG_DISPLAY_INIT:
        err = vga32_start_signal(display);
        break;
    case U8X8_MSG_DISPLAY_SET_POWER_SAVE:
        if (arg_int != 0U) {
            vga32_stop_signal(display);
        } else {
            err = vga32_start_signal(display);
        }
        break;
    case U8X8_MSG_DISPLAY_DRAW_TILE:
        return 1;
    case U8X8_MSG_DISPLAY_REFRESH:
        err = vga32_present(display);
        break;
    default:
        return 0;
    }
    display->last_error = err;
    return err == ESP_OK ? 1 : 0;
}

esp_err_t vga32_init(vga32_t *display, const vga32_config_t *config)
{
    if (display == NULL || config == NULL ||
        config->red0_pin < 0 || config->red1_pin < 0 ||
        config->green0_pin < 0 || config->green1_pin < 0 ||
        config->blue0_pin < 0 || config->blue1_pin < 0 ||
        config->hsync_pin < 0 || config->vsync_pin < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(display, 0, sizeof(*display));
    display->config = *config;
    if (display->config.rotation == NULL) {
        display->config.rotation = U8G2_R1;
    }
    display->buffer_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    display->present_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    display->current_buffer = 0;
    display->pending_buffer = -1;
    display->copying_buffer = -1;
    display->pending_present_buffer = -1;
    display->rendering_present_buffer = -1;
    display->copying_present_buffer = -1;
    display->foreground = 0x00U;
    display->background = VGA32_COLOR_MASK;
    vga32_rebuild_pixel_lut(display);
    display->draw_buffer_size = VGA32_DRAW_BUFFER_SIZE;
    display->scanout_buffer_size = VGA32_SCANOUT_BUFFER_SIZE;
    display->draw_buffer = heap_caps_calloc(
        1,
        display->draw_buffer_size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for (size_t i = 0; i < VGA32_SCANOUT_BUFFER_COUNT; i++) {
        display->scanout_buffers[i] = heap_caps_malloc(
            display->scanout_buffer_size,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (display->scanout_buffers[i] != NULL) {
            memset(display->scanout_buffers[i],
                   0xFF,
                   display->scanout_buffer_size);
        }
    }
    if (display->draw_buffer == NULL || display->scanout_buffers[0] == NULL
#if VGA32_SCANOUT_BUFFER_COUNT > 1U
        || display->scanout_buffers[1] == NULL
#endif
    ) {
        vga32_deinit(display);
        return ESP_ERR_NO_MEM;
    }

    bool present_buffers_ready = true;
    for (size_t i = 0; i < VGA32_PRESENT_BUFFER_COUNT; i++) {
        display->present_buffers[i] = heap_caps_malloc(
            display->draw_buffer_size,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        present_buffers_ready = present_buffers_ready &&
            display->present_buffers[i] != NULL;
    }
    if (!present_buffers_ready) {
        ESP_LOGW(TAG,
                 "asynchronous present unavailable; using synchronous fallback: %s",
                 esp_err_to_name(ESP_ERR_NO_MEM));
        for (size_t i = 0; i < VGA32_PRESENT_BUFFER_COUNT; i++) {
            heap_caps_free(display->present_buffers[i]);
            display->present_buffers[i] = NULL;
        }
    }

    u8g2_SetupDisplay(&display->u8g2,
                      vga32_u8x8_display_cb,
                      u8x8_cad_empty,
                      u8x8_dummy_cb,
                      u8x8_dummy_cb);
    u8g2_SetupBuffer(&display->u8g2,
                     display->draw_buffer,
                     VGA32_TILE_HEIGHT,
                     u8g2_ll_hvline_vertical_top_lsb,
                     display->config.rotation);
    active_display = display;
    u8g2_InitDisplay(&display->u8g2);
    if (display->last_error != ESP_OK) {
        const esp_err_t err = display->last_error;
        vga32_deinit(display);
        return err;
    }
    u8g2_ClearBuffer(&display->u8g2);
    return ESP_OK;
}

esp_err_t vga32_start_async_present(vga32_t *display)
{
    if (display == NULL || display->draw_buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&display->present_lock);
    const bool already_started = display->present_task != NULL;
    const bool stopping = display->present_stop_requested;
    portEXIT_CRITICAL(&display->present_lock);
    if (already_started) {
        return ESP_OK;
    }
    if (stopping) {
        return ESP_ERR_INVALID_STATE;
    }
    return vga32_start_present_task(display);
}

esp_err_t vga32_resume(vga32_t *display)
{
    if (display == NULL || display->draw_buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    active_display = display;
    display->last_error = vga32_start_signal(display);
    return display->last_error;
}

void vga32_deinit(vga32_t *display)
{
    if (display == NULL) {
        return;
    }
    vga32_stop_signal(display);
    vga32_stop_present_task(display);
    if (active_display == display) {
        active_display = NULL;
    }
    heap_caps_free(display->draw_buffer);
    display->draw_buffer = NULL;
    for (size_t i = 0; i < VGA32_SCANOUT_BUFFER_COUNT; i++) {
        heap_caps_free(display->scanout_buffers[i]);
        display->scanout_buffers[i] = NULL;
    }
    for (size_t i = 0; i < VGA32_PRESENT_BUFFER_COUNT; i++) {
        heap_caps_free(display->present_buffers[i]);
        display->present_buffers[i] = NULL;
    }
}

u8g2_t *vga32_get_u8g2(vga32_t *display)
{
    return display != NULL ? &display->u8g2 : NULL;
}

esp_err_t vga32_set_colors(vga32_t *display,
                           uint32_t foreground_rgb888,
                           uint32_t background_rgb888)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    display->foreground = vga32_rgb222(foreground_rgb888);
    display->background = vga32_rgb222(background_rgb888);
    vga32_rebuild_pixel_lut(display);
    return ESP_OK;
}

esp_err_t vga32_present_mono_xbm(vga32_t *display,
                                 const uint8_t *bitmap,
                                 size_t bitmap_size,
                                 uint16_t x,
                                 uint16_t y,
                                 uint16_t width,
                                 uint16_t height,
                                 uint16_t stride,
                                 bool palette_inverted)
{
    const size_t source_bytes = ((size_t)width + 7U) / 8U;
    if (display == NULL || bitmap == NULL || width == 0U || height == 0U ||
        stride < source_bytes || height > SIZE_MAX / stride ||
        bitmap_size < (size_t)height * stride ||
        (uint32_t)x + width > VGA32_WIDTH ||
        (uint32_t)y + height > VGA32_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }

    const int8_t target = vga32_acquire_copy_buffer(display);
    if (target < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t *scanout = display->scanout_buffers[target];
    memset(scanout,
           palette_inverted ? 0x00 : 0xFF,
           display->scanout_buffer_size);
    for (size_t source_y = 0; source_y < height; source_y++) {
        const uint8_t *source = bitmap + source_y * stride;
        uint8_t *destination =
            &scanout[((size_t)y + source_y) * VGA32_SCANOUT_STRIDE];
        for (size_t source_x = 0; source_x < width; source_x++) {
            const bool black =
                (source[source_x / 8U] & (1U << (source_x % 8U))) != 0U;
            const bool background = black == palette_inverted;
            const size_t output_x = (size_t)x + source_x;
            const uint8_t mask = (uint8_t)(1U << (output_x % 8U));
            if (background) {
                destination[output_x / 8U] |= mask;
            } else {
                destination[output_x / 8U] &= (uint8_t)~mask;
            }
        }
    }
    vga32_commit_copy_buffer(display, target);
    return ESP_OK;
}

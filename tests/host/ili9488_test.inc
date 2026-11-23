#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tft_ili9341.h"
#define portMAX_DELAY 0xffffffffU
static tft_ili9341_t *active_display;
static spi_transaction_t *queued[2];
static unsigned queue_count, submitted, fail_at;
static uint8_t sent[4096];
static size_t sent_bytes;
static void solar_os_vector_fill_rgb565_be(uint8_t *data, uint16_t color, size_t n)
{ for (size_t i = 0; i < n; ++i) { data[2*i] = color >> 8; data[2*i+1] = color; } }
static esp_err_t spi_device_queue_trans(spi_device_handle_t device, spi_transaction_t *t, unsigned wait)
{
    (void)device; (void)wait; assert(queue_count < 2 && t->rxlength == 0);
    if (++submitted == fail_at) return ESP_FAIL;
    t->rxlength = t->length; queued[queue_count++] = t; return ESP_OK;
}
static esp_err_t spi_device_get_trans_result(spi_device_handle_t device, spi_transaction_t **out, unsigned wait)
{
    (void)device; (void)wait; assert(queue_count);
    *out = queued[0]; size_t bytes = (*out)->length / 8;
    assert(bytes <= sizeof(sent) - sent_bytes);
    memcpy(sent + sent_bytes, (*out)->tx_buffer, bytes); sent_bytes += bytes;
    queued[0] = queued[1]; --queue_count; return ESP_OK;
}
/* FUNCTIONS */
static uint16_t color(unsigned row, unsigned col) { return (uint16_t)(0x1234U + row * 921U + col * 227U); }
static void render(tft_ili9341_t *display, const void *ctx, uint16_t row, uint16_t width, uint8_t *out)
{
    (void)display; (void)ctx;
    /* RGB565 renderers require two-byte alignment, even for odd wire widths. */
    assert((uintptr_t)out % 2 == 0);
    for (size_t x = 0; x < width; ++x) {
        ((uint16_t *)out)[x] = __builtin_bswap16(color(row, x));
    }
}
static void expected(uint16_t color, uint8_t out[3])
{
    unsigned r = (color >> 11) & 31, g = (color >> 5) & 63, b = color & 31;
    out[0] = ((r * 2) + r / 16) * 4; out[1] = g * 4; out[2] = ((b * 2) + b / 16) * 4;
}
int main(void)
{
    uint8_t *all = malloc(65536 * 3 + 8); assert(all);
    for (unsigned p = 0; p < 65536; ++p) { all[2*p] = p >> 8; all[2*p+1] = p; }
    memset(all + 65536 * 3, 0xa5, 8);
    ili9488_expand_rgb565(all, 65536);
    for (unsigned p = 0; p < 65536; ++p) { uint8_t want[3]; expected(p, want); assert(!memcmp(all + p*3, want, 3)); }
    for (unsigned p = 0; p < 8; ++p) assert(all[65536*3+p] == 0xa5);
    ili9488_expand_rgb565(all, 0); free(all);
    for (unsigned rgb666 = 0; rgb666 <= 1; ++rgb666) {
        for (unsigned width = 1; width <= 17; width += 2) {
            tft_ili9341_t d = {.config = {.ili9488 = rgb666}};
            d.line_buffer_size = width * (rgb666 ? 3 : 2) * 4;
            d.line_buffer = malloc(d.line_buffer_size + 4);
            d.line_buffer_alt = malloc(d.line_buffer_size + 4);
            assert(d.line_buffer && d.line_buffer_alt);
            memset(d.line_buffer + d.line_buffer_size, 0xa5, 4);
            memset(d.line_buffer_alt + d.line_buffer_size, 0xa5, 4);
            sent_bytes = queue_count = submitted = fail_at = 0;
            assert(ili9341_transmit_rendered_lines(&d, width, 9, render, NULL) == ESP_OK);
            assert(sent_bytes == width * 9 * (rgb666 ? 3 : 2) && submitted == 3 && !queue_count);
            for (unsigned row = 0; row < 9; ++row) for (unsigned col = 0; col < width; ++col) {
                uint8_t want[3]; expected(color(row, col), want);
                size_t pos = (row * width + col) * (rgb666 ? 3 : 2);
                if (rgb666) assert(!memcmp(sent + pos, want, 3));
                else assert(sent[pos] == color(row, col) >> 8 && sent[pos+1] == (uint8_t)color(row, col));
            }
            ili9341_fill_line(&d, 0xffff, width);
            for (unsigned p = 0; p < width * (rgb666 ? 3 : 2); ++p) assert(d.line_buffer[p] == (rgb666 ? 0xfc : 0xff));
            for (unsigned p = 0; p < 4; ++p) {
                assert(d.line_buffer[d.line_buffer_size+p] == 0xa5);
                assert(d.line_buffer_alt[d.line_buffer_size+p] == 0xa5);
            }
            for (fail_at = 1; fail_at <= 3; ++fail_at) {
                sent_bytes = queue_count = submitted = 0;
                assert(ili9341_transmit_rendered_lines(&d, width, 9, render, NULL) == ESP_FAIL);
                assert(!queue_count);
            }
            free(d.line_buffer); free(d.line_buffer_alt);
        }
    }
    tft_ili9341_t first = {0}, second = {0}; active_display = &second;
    first.u8g2.u8x8.user_ptr = (void *)1; /* Geometry owns the user pointer. */
    assert(ili9341_from_u8x8(&first.u8g2.u8x8) == &first);
    assert(ili9341_from_u8x8(&second.u8g2.u8x8) == &second);
    assert(first.u8g2.u8x8.user_ptr == (void *)1);
    puts("ILI9488: RGB666 conversion, odd-width DMA bands, RGB565 regression, failure draining and independent panel contexts passed");
}

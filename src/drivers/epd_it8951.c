#include "epd_it8951.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "solar_os_memory.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "solar_os_buses.h"

#define WIDTH SOLAR_OS_IT8951_WIDTH
#define HEIGHT SOLAR_OS_IT8951_HEIGHT
#define MONO_BYTES ((size_t)WIDTH * ((HEIGHT + 7U) / 8U))
#define GRAY_BYTES ((size_t)WIDTH * HEIGHT / 2U)
#define TRANSFER_BYTES 4096U
#define MAX_REGIONS 32U
#define TRY(call) do { esp_err_t err_ = (call); if (err_ != ESP_OK) return err_; } while (0)

static const char *TAG = "it8951";
static const u8x8_display_info_t display_info = {
    .chip_enable_level = 0, .chip_disable_level = 1,
    .sck_clock_hz = 10000000, .spi_mode = 0,
    .tile_width = (WIDTH + 7U) / 8U, .tile_height = (HEIGHT + 7U) / 8U,
    .pixel_width = WIDTH, .pixel_height = HEIGHT,
};

static void delay_ms(unsigned ms)
{
    const TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

static esp_err_t wait_ready(solar_os_it8951_t *d)
{
    const int64_t deadline = esp_timer_get_time() + 2000000;
    while (!gpio_get_level(d->config.busy)) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        delay_ms(1);
    }
    return ESP_OK;
}

/* CS is managed explicitly across preamble, ready waits, dummy and payload.
 * Acquire the shared SPI bus only for this transaction, never while refreshing. */
static esp_err_t transfer(solar_os_it8951_t *d, uint16_t preamble,
                           const uint8_t *tx, uint8_t *rx, size_t size)
{
    TRY(wait_ready(d));
    TRY(spi_device_acquire_bus(d->spi, portMAX_DELAY));
    esp_err_t err = gpio_set_level(d->config.cs, 0);
    const uint8_t header[] = {(uint8_t)(preamble >> 8), (uint8_t)preamble};
    spi_transaction_t t = {.length = 16, .tx_buffer = header};
    if (err == ESP_OK) err = spi_device_polling_transmit(d->spi, &t);
    if (err == ESP_OK) err = wait_ready(d);
    if (err == ESP_OK && rx) {
        const uint8_t dummy[2] = {0};
        t = (spi_transaction_t){.length = 16, .tx_buffer = dummy};
        err = spi_device_polling_transmit(d->spi, &t);
        if (err == ESP_OK) err = wait_ready(d);
    }
    if (rx) {
        /* The controller can pause between response words. At 20 MHz a
         * continuous device-info read can repeat words before the next one
         * is ready. Keep CS asserted and honor HRDY for each 16-bit word. */
        for (size_t offset = 0; err == ESP_OK && offset < size; offset += 2U) {
            const size_t count = size - offset < 2U ? size - offset : 2U;
            err = wait_ready(d);
            if (err == ESP_OK) {
                t = (spi_transaction_t){.length = count * 8U, .rx_buffer = rx + offset};
                err = spi_device_polling_transmit(d->spi, &t);
            }
        }
    } else if (err == ESP_OK) {
        t = (spi_transaction_t){.length = size * 8U, .tx_buffer = tx};
        /* Short commands stay polling; bulk DMA sleeps this worker until the
         * interrupt completes. Drain it before changing CS or releasing SD. */
        err = size >= 64U ? spi_device_transmit(d->spi, &t) :
            spi_device_polling_transmit(d->spi, &t);
    }
    const esp_err_t cs_err = gpio_set_level(d->config.cs, 1);
    spi_device_release_bus(d->spi);
    return err == ESP_OK ? cs_err : err;
}

static esp_err_t word(solar_os_it8951_t *d, uint16_t preamble, uint16_t value)
{
    const uint8_t bytes[] = {(uint8_t)(value >> 8), (uint8_t)value};
    return transfer(d, preamble, bytes, NULL, sizeof(bytes));
}

static esp_err_t command(solar_os_it8951_t *d, uint16_t cmd,
                          const uint16_t *args, size_t count)
{
    TRY(word(d, 0x6000, cmd));
    for (size_t i = 0; i < count; i++) TRY(word(d, 0, args[i]));
    return ESP_OK;
}

static esp_err_t write_reg(solar_os_it8951_t *d, uint16_t reg, uint16_t value)
{
    const uint16_t args[] = {reg, value};
    return command(d, 0x0011, args, 2);
}

static esp_err_t read_reg(solar_os_it8951_t *d, uint16_t reg, uint16_t *value)
{
    TRY(command(d, 0x0010, &reg, 1));
    uint8_t data[2];
    TRY(transfer(d, 0x1000, NULL, data, sizeof(data)));
    *value = (uint16_t)((uint16_t)data[0] << 8) | data[1];
    return ESP_OK;
}

static esp_err_t wait_lut(solar_os_it8951_t *d)
{
    const int64_t deadline = esp_timer_get_time() + 30000000;
    for (;;) {
        uint16_t busy;
        TRY(read_reg(d, 0x1224, &busy));
        if (!busy) return ESP_OK;
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        delay_ms(10);
    }
}

static esp_err_t rails(solar_os_it8951_t *d, bool on)
{
    const esp_err_t panel_err = gpio_set_level(d->config.power, on);
    const esp_err_t controller_err = gpio_set_level(d->config.ite_power, on);
    if (panel_err != ESP_OK || controller_err != ESP_OK) {
        d->ready = d->shadow_valid = false;
        /* Attempt both disables even if one GPIO operation failed. */
        const esp_err_t panel_off = gpio_set_level(d->config.power, 0);
        const esp_err_t controller_off = gpio_set_level(d->config.ite_power, 0);
        d->powered = panel_off != ESP_OK || controller_off != ESP_OK;
        return panel_err != ESP_OK ? panel_err : controller_err;
    }
    d->powered = on;
    if (!on) d->ready = d->shadow_valid = false;
    return ESP_OK;
}

static esp_err_t update_temperature(solar_os_it8951_t *d)
{
    const int64_t now = esp_timer_get_time();
    if (d->temperature_valid && now < d->temperature_due) return ESP_OK;
    int16_t temperature = d->temperature_c;
    bool measured = false;
    if (d->config.read_temperature) {
        int16_t sample;
        if (d->config.read_temperature(&sample) == ESP_OK && sample >= 0 && sample <= 50) {
            temperature = sample;
            measured = true;
        }
    }
    /* Retry promptly if a provider is not yet attached during early boot.
     * Otherwise sample at most every 30 seconds, outside the shared SPI lock. */
    d->temperature_due = now + (measured ? 30000000 : 1000000);
    if (!d->temperature_valid || temperature != d->temperature_c) {
        const uint16_t args[] = {1,(uint16_t)temperature};
        TRY(command(d,0x0040,args,2));
        d->temperature_c = temperature;
        d->temperature_valid = true;
    }
    return ESP_OK;
}

static void device_info_string(char out[17], const uint8_t *raw)
{
    size_t n = 0;
    while (n < 16U && raw[n] && raw[n] != 0xffU) {
        out[n] = raw[n] >= 32U && raw[n] <= 126U ? (char)raw[n] : '?';
        ++n;
    }
    while (n && out[n-1U] == ' ') --n;
    out[n] = '\0';
}

static esp_err_t controller_init(solar_os_it8951_t *d)
{
    d->ready = d->shadow_valid = false;
    d->temperature_valid = false;
    TRY(rails(d, true));
    delay_ms(50);
    TRY(gpio_set_level(d->config.reset, 0));
    delay_ms(10);
    TRY(gpio_set_level(d->config.reset, 1));
    delay_ms(50);
    TRY(command(d, 0x0001, NULL, 0));
    TRY(command(d, 0x0302, NULL, 0));
    uint8_t info[40];
    TRY(transfer(d, 0x1000, NULL, info, sizeof(info)));
    const unsigned width = ((unsigned)info[0] << 8) | info[1];
    const unsigned height = ((unsigned)info[2] << 8) | info[3];
    if (width != WIDTH || height != HEIGHT) {
        ESP_LOGE(TAG, "unexpected panel %ux%u (expected %ux%u)", width, height, WIDTH, HEIGHT);
        return ESP_ERR_INVALID_RESPONSE;
    }
    d->image_address = ((uint32_t)info[6] << 24) | ((uint32_t)info[7] << 16) |
                       ((uint32_t)info[4] << 8) | info[5];
    if (!d->image_address || d->image_address == UINT32_MAX) return ESP_ERR_INVALID_RESPONSE;
    device_info_string(d->firmware_version,info+8);
    device_info_string(d->waveform_version,info+24);
    const uint16_t vcom[] = {2, d->config.vcom_mv};
    TRY(command(d, 0x0039, vcom, 2));
    TRY(write_reg(d, 0x0004, 1)); /* packed SPI host interface */
    /* The host must declare temperature: this panel has auto sensing disabled.
     * Use the environment provider when present, retaining a 25 C fallback. */
    TRY(update_temperature(d));
    d->ready = true;
    d->updates = 0;
    ESP_LOGI(TAG, "ED103TC2 %ux%u image=0x%08lx VCOM=-%umV fw=%s lut=%s",
             WIDTH, HEIGHT, (unsigned long)d->image_address, d->config.vcom_mv,
             d->firmware_version,d->waveform_version);
    return ESP_OK;
}

static uint8_t gray_at(const uint8_t *image, unsigned x, unsigned y)
{
    const uint8_t b = image[(size_t)y * (WIDTH / 2U) + x / 2U];
    return (x & 1U) ? b & 15U : b >> 4;
}

static bool set_pixel(solar_os_it8951_t *d, unsigned x, unsigned y, uint8_t gray)
{
    uint8_t *image = d->gray_pending ? d->gray_pending : d->gray;
    uint8_t *b = &image[(size_t)y * (WIDTH / 2U) + x / 2U];
    const uint8_t old = gray_at(image,x,y);
    const bool changed = old != gray;
    *b = (x & 1U) ? (uint8_t)((*b & 0xf0U) | gray) :
                    (uint8_t)((*b & 0x0fU) | (gray << 4));
    b = &d->mono[(size_t)(y / 8U) * WIDTH + x];
    const uint8_t mask = (uint8_t)(1U << (y & 7U));
    if (gray >= 8U) *b |= mask;
    else *b &= (uint8_t)~mask;
    return changed;
}

typedef struct { unsigned x0,x1,y0,y1; uint64_t *rows; } dirty_area_t;

static void dirty_include(dirty_area_t *area,unsigned x,unsigned y,unsigned width)
{
    if (x < area->x0) area->x0 = x;
    if (x+width > area->x1) area->x1 = x+width;
    if (y < area->y0) area->y0 = y;
    if (y+1U > area->y1) area->y1 = y+1U;
    if (area->rows) {
        /* The controller mirrors X. Track its 32-pixel source boundaries. */
        const unsigned first = (WIDTH-x-width)/32U, last = (WIDTH-1U-x)/32U;
        area->rows[y] |= ((UINT64_C(1) << (last-first+1U))-1U) << first;
    }
}

static dirty_area_t mono_to_gray_area(solar_os_it8951_t *d, const uint8_t *source,
    const dirty_area_t *area)
{
    dirty_area_t changed = {WIDTH,0,HEIGHT,0,d->dirty_rows};
    memset(d->dirty_rows,0,HEIGHT*sizeof(*d->dirty_rows));
    const unsigned x0 = area->x0 & ~1U, x1 = (area->x1+1U) & ~1U;
    for (unsigned y = area->y0; y < area->y1; y++) {
        const uint8_t *row = source + (size_t)(y / 8U) * WIDTH;
        const uint8_t mask = (uint8_t)(1U << (y & 7U));
        for (unsigned x = x0; x < x1; x += 2U) {
            const size_t i = (size_t)y*(WIDTH/2U)+x/2U;
            const uint8_t b = (uint8_t)((row[x] & mask ? 0xf0 : 0) |
                (row[x+1U] & mask ? 0x0f : 0));
            if (d->gray[i] != b) {
                d->gray[i] = b;
                dirty_include(&changed,x,y,2);
            }
        }
    }
    return changed;
}

static dirty_area_t mono_to_gray(solar_os_it8951_t *d, const uint8_t *source)
{
    const dirty_area_t full = {0,WIDTH,0,HEIGHT,NULL};
    return mono_to_gray_area(d,source,&full);
}

static dirty_area_t mono_dirty_area(solar_os_it8951_t *d, const uint8_t *source)
{
    dirty_area_t area = {WIDTH,0,HEIGHT,0,NULL};
    if (!d->mono_shadow_valid || !d->shadow_valid)
        return (dirty_area_t){0,WIDTH,0,HEIGHT,NULL};
    for (unsigned y = 0; y < HEIGHT; y += 8U) {
        const size_t offset = (size_t)(y/8U)*WIDTH;
        if (!memcmp(source+offset,d->mono_shadow+offset,WIDTH)) continue;
        const unsigned end = y+8U > HEIGHT ? HEIGHT : y+8U;
        const uint8_t mask = (uint8_t)((1U << (end-y))-1U);
        for (unsigned x = 0; x < WIDTH; x++) {
            if (!((source[offset+x]^d->mono_shadow[offset+x]) & mask)) continue;
            if (x < area.x0) area.x0 = x;
            if (x+1U > area.x1) area.x1 = x+1U;
            if (y < area.y0) area.y0 = y;
            area.y1 = end;
        }
    }
    return area;
}

/* Four packed binary-gray bytes become eight pixels in native X order.
 * Both nibbles are either 0 or 15; keep their top bits in the output. */
static uint8_t pack_binary_byte(const uint8_t *source)
{
    return (source[0] & 0x80U) | ((source[0] & 0x08U) << 3) |
        ((source[1] & 0x80U) >> 2) | ((source[1] & 0x08U) << 1) |
        ((source[2] & 0x80U) >> 4) | ((source[2] & 0x08U) >> 1) |
        ((source[3] & 0x80U) >> 6) | ((source[3] & 0x08U) >> 3);
}

typedef struct { unsigned x,y,width,height; } upload_area_t;

static unsigned row_bpp(const uint8_t *row)
{
    for (size_t i = 0; i < WIDTH/2U; i += sizeof(uint32_t)) {
        uint32_t b;
        memcpy(&b,row+i,sizeof(b));
        if ((b & 0x11111111U)*15U != b) return 4;
    }
    return 1;
}

static unsigned image_bpp(solar_os_it8951_t *d, bool known_binary, const dirty_area_t *dirty)
{
    if (known_binary) memset(d->row_bpp,1,HEIGHT);
    else {
        /* A small patch must not rescan 1.3 MB merely to choose its encoding.
         * The cache describes original shades, independent of output policy. */
        for (unsigned y = 0; y < HEIGHT; y++) {
            if (!d->row_bpp_valid || !dirty->rows || dirty->rows[y])
                d->row_bpp[y] = row_bpp(d->gray+(size_t)y*(WIDTH/2U));
        }
    }
    d->row_bpp_valid = true;
    unsigned bpp = 1;
    for (unsigned y = 0; y < HEIGHT; y++) if (d->row_bpp[y] > bpp) bpp = d->row_bpp[y];
    return bpp;
}

/* IT8951 zero-pads 2-bit input to levels 0,4,8,12, so it cannot encode white.
 * Four-level output must send 0,5,10,15 through the normal 4-bit path. */
#define PAIR4(h,l) (((((l)+2U)/5U)*5U << 4) | ((((h)+2U)/5U)*5U))
#define ROW4(h) PAIR4(h,0),PAIR4(h,1),PAIR4(h,2),PAIR4(h,3), \
    PAIR4(h,4),PAIR4(h,5),PAIR4(h,6),PAIR4(h,7), \
    PAIR4(h,8),PAIR4(h,9),PAIR4(h,10),PAIR4(h,11), \
    PAIR4(h,12),PAIR4(h,13),PAIR4(h,14),PAIR4(h,15)
static const uint8_t gray_pairs_four_level[256] = {
    ROW4(0),ROW4(1),ROW4(2),ROW4(3),ROW4(4),ROW4(5),ROW4(6),ROW4(7),
    ROW4(8),ROW4(9),ROW4(10),ROW4(11),ROW4(12),ROW4(13),ROW4(14),ROW4(15)
};
#undef ROW4
#undef PAIR4

/* Merge matching horizontal runs vertically. Bound command overhead and fall
 * back to the bounding rectangle for fragmented or dense damage. */
static unsigned upload_regions(const dirty_area_t *dirty, const upload_area_t *bounds,
    bool clean, upload_area_t areas[MAX_REGIONS])
{
    unsigned count = 0;
    size_t pixels = 0;
    if (!clean && dirty->rows) {
        for (unsigned y = dirty->y0; y < dirty->y1; y++) {
            uint64_t bits = dirty->rows[y];
            while (bits) {
                const unsigned first = (unsigned)__builtin_ctzll(bits);
                unsigned end = first+1U;
                while (bits & (UINT64_C(1) << end)) ++end;
                const uint64_t mask = ((UINT64_C(1) << (end-first))-1U) << first;
                bits &= ~mask;
                const unsigned x = first*32U;
                const unsigned right = end*32U < WIDTH ? end*32U : WIDTH;
                unsigned i;
                for (i = 0; i < count; i++) {
                    if (areas[i].x == x && areas[i].width == right-x &&
                        areas[i].y+areas[i].height == y) break;
                }
                if (i == count) {
                    if (count == MAX_REGIONS) goto bounding;
                    areas[count++] = (upload_area_t){x,y,right-x,0};
                }
                ++areas[i].height;
                pixels += right-x;
            }
        }
        if (count && pixels < (size_t)bounds->width*bounds->height) return count;
    }
bounding:
    areas[0] = *bounds;
    return 1;
}

static esp_err_t upload_area(solar_os_it8951_t *d, const upload_area_t *area, unsigned bpp)
{
    const bool binary = bpp == 1;
    const uint16_t load[] = {binary ? 0x0030 : 0x0120,
        binary ? area->x/8U : area->x, area->y,
        binary ? area->width/8U : area->width, area->height};
    TRY(command(d,0x0021,load,5));
    const size_t row_bytes = (size_t)area->width*bpp/8U;
    unsigned rows_per_chunk = d->transfer_size/row_bytes;
    /* Pair 234-byte full-width binary rows to avoid DMA bounce buffers. */
    if ((row_bytes & 3U) && rows_per_chunk > 1U) rows_per_chunk &= ~1U;
    for (unsigned y = area->y; y < area->y+area->height;) {
        unsigned lines = area->y+area->height-y;
        if (lines > rows_per_chunk) lines = rows_per_chunk;
        for (unsigned row = 0; row < lines; row++) {
            const uint8_t *source = d->gray+(size_t)(y+row)*(WIDTH/2U)+(WIDTH-area->x)/2U;
            uint8_t *out = d->transfer+row*row_bytes;
            if (binary) {
                for (unsigned x = 0; x < area->width; x += 16U) {
                    source -= 8;
                    out[x/8U] = pack_binary_byte(source);
                    out[x/8U+1U] = pack_binary_byte(source+4);
                }
            } else {
                for (size_t x = 0; x < row_bytes; x++) {
                    const uint8_t pair = *--source;
                    out[x] = d->gray_levels == 4 ? gray_pairs_four_level[pair] :
                        (uint8_t)((pair << 4) | (pair >> 4));
                }
            }
        }
        const size_t bytes = lines*row_bytes;
        TRY(transfer(d,0,d->transfer,NULL,bytes));
        d->last_upload_bytes += bytes;
        ++d->last_chunks;
        y += lines;
    }
    return command(d,0x0022,NULL,0);
}

static esp_err_t refresh(solar_os_it8951_t *d, bool known_binary,
    const dirty_area_t *dirty)
{
    const int64_t started = esp_timer_get_time();
    if (!d->ready) TRY(controller_init(d));
    /* Conversion tracks changes against the retained packed image, so no
     * second full grayscale image is needed. Bounds use native coordinates. */
    unsigned x0 = dirty->x0, x1 = dirty->x1, y0 = dirty->y0, y1 = dirty->y1;
    if (solar_os_epd_native_plan(&d->maintenance, x1 != 0, d->shadow_valid,
        d->full_refresh, false, false).action == SOLAR_OS_EPD_KEEP) return ESP_OK;
    const unsigned bpp = image_bpp(d,known_binary,dirty);
    const bool binary = bpp == 1;
    /* Changing the framebuffer encoding requires a complete upload. GC16 also
     * clears the old grayscale pixels before subsequent binary DU updates. */
    const solar_os_epd_plan_t plan = solar_os_epd_native_plan(&d->maintenance,
        x1 != 0, d->shadow_valid, d->full_refresh, bpp != d->shadow_bpp, d->updates >= 19U);
    const bool clean = plan.action == SOLAR_OS_EPD_FULL;
    if (clean) { x0 = 0; x1 = WIDTH; y0 = 0; y1 = HEIGHT; }
    const unsigned mirrored_end = WIDTH - x0;
    x0 = (WIDTH - x1) & ~31U;
    x1 = (mirrored_end + 31U) & ~31U;
    if (x1 > WIDTH) x1 = WIDTH;
    const unsigned width = x1 - x0, height = y1 - y0;
    const upload_area_t bounds = {x0,y0,width,height};
    upload_area_t areas[MAX_REGIONS];
    const unsigned count = upload_regions(dirty,&bounds,clean,areas);
    TRY(wait_lut(d));
    TRY(update_temperature(d));
    TRY(write_reg(d, 0x020a, (uint16_t)(d->image_address >> 16)));
    TRY(write_reg(d, 0x0208, (uint16_t)d->image_address));
    /* Multiple image loads update retained controller RAM before one display
     * command. Release the shared SD bus after each bounded bulk transfer. */
    d->last_upload_bytes = d->last_chunks = 0;
    d->last_regions = count;
    for (unsigned i = 0; i < count; i++) TRY(upload_area(d,&areas[i],bpp));
    uint16_t up1sr;
    TRY(read_reg(d, 0x113a, &up1sr));
    TRY(write_reg(d, 0x113a, binary ? up1sr | 4U : up1sr & ~4U));
    if (binary) TRY(write_reg(d, 0x1250, 0x00ff)); /* 0=black, 1=white */
    const bool full = clean || !binary;
    const uint16_t area[] = {x0, y0, width, height, full ? 2 : 1};
    const int64_t uploaded = esp_timer_get_time();
    TRY(command(d, 0x0034, area, 5));
    TRY(wait_lut(d));
    const int64_t finished = esp_timer_get_time();
    d->shadow_valid = true;
    d->shadow_bpp = bpp;
    d->updates = full ? 0 : d->updates + 1U;
    if (clean) solar_os_epd_note_cleanup(&d->maintenance, plan.reason);
    d->last_x = x0; d->last_y = y0;
    d->last_width = width; d->last_height = height; d->last_bpp = bpp;
    d->last_upload_ms = (unsigned)((uploaded - started) / 1000);
    d->last_lut_ms = (unsigned)((finished - uploaded) / 1000);
    d->last_refresh_ms = (unsigned)((esp_timer_get_time() - started) / 1000);
    return ESP_OK;
}

static esp_err_t finish_refresh(solar_os_it8951_t *d, bool known_binary,
    const dirty_area_t *dirty)
{
    const esp_err_t err = refresh(d,known_binary,dirty);
    if (err != ESP_OK) {
        /* A partially transferred frame must never become the comparison baseline. */
        d->ready = d->shadow_valid = false;
        ESP_LOGE(TAG, "refresh failed: %s", esp_err_to_name(err));
    }
    d->last_error = err;
    return err;
}

static void scan_enable(solar_os_it8951_t *d, bool enabled)
{
    enabled = enabled && d->scan_worker.task != NULL;
    __atomic_store_n(&d->scan_submit_ready,enabled,__ATOMIC_RELEASE);
    epd_scan_worker_enable(&d->scan_worker,enabled);
}

/* Join before changing presentation formats, mode or power. Merely disabling
 * submissions cannot cancel a callback already waiting for the panel lock. */
static void scan_stop(solar_os_it8951_t *d)
{
    scan_enable(d,false);
    epd_scan_worker_deinit(&d->scan_worker);
    solar_os_memory_free(d->mono_shadow); d->mono_shadow = NULL;
    solar_os_memory_free(d->gray_pending); d->gray_pending = NULL;
    solar_os_memory_free(d->pending_rows); d->pending_rows = NULL;
    d->mono_shadow_valid = false;
    d->graphics_scan = false;
}

/* The producer lock protects only image composition/copying. Never retain it
 * during SPI or waveform waits: the foreground must be able to replace the
 * queued target while the panel is busy. */
static dirty_area_t take_graphics_target(solar_os_it8951_t *d)
{
    xSemaphoreTake(d->frame_mutex,portMAX_DELAY);
    dirty_area_t dirty = {WIDTH,0,HEIGHT,0,d->dirty_rows};
    memset(d->dirty_rows,0,HEIGHT*sizeof(*d->dirty_rows));
    if (d->pending_x1) {
        for (unsigned y = d->pending_y0; y < d->pending_y1; y++) {
            uint64_t bits = d->pending_rows[y];
            d->pending_rows[y] = 0;
            while (bits) {
                const unsigned column = (unsigned)__builtin_ctzll(bits);
                bits &= bits-1U;
                const unsigned right = (column+1U)*32U < WIDTH ? (column+1U)*32U : WIDTH;
                const unsigned x = WIDTH-right, width = right-column*32U;
                const size_t offset = (size_t)y*(WIDTH/2U)+x/2U;
                if (!memcmp(d->gray+offset,d->gray_pending+offset,width/2U)) continue;
                memcpy(d->gray+offset,d->gray_pending+offset,width/2U);
                dirty_include(&dirty,x,y,width);
            }
        }
    }
    d->pending_x0 = WIDTH; d->pending_x1 = 0;
    d->pending_y0 = HEIGHT; d->pending_y1 = 0;
    xSemaphoreGive(d->frame_mutex);
    return dirty;
}

static esp_err_t scan_process(void *context, const uint8_t *source,
    bool fresh, bool *pending, bool *scanned)
{
    solar_os_it8951_t *d = context;
    *pending = *scanned = false;
    xSemaphoreTake(d->mutex,portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if ((fresh || d->full_refresh) && !d->suspended &&
        __atomic_load_n(&d->scan_submit_ready,__ATOMIC_ACQUIRE)) {
        dirty_area_t dirty;
        if (d->graphics_scan) dirty = take_graphics_target(d);
        else {
            dirty = mono_dirty_area(d,source);
            if (dirty.x1) dirty = mono_to_gray_area(d,source,&dirty);
        }
        err = finish_refresh(d,!d->graphics_scan,&dirty);
        *scanned = err == ESP_OK;
        if (err != ESP_OK) {
            /* Retry once after controller reset, even if the app stops painting. */
            err = finish_refresh(d,!d->graphics_scan,&dirty);
            *scanned = err == ESP_OK;
            if (err != ESP_OK) __atomic_store_n(&d->scan_submit_ready,false,__ATOMIC_RELEASE);
        }
        if (err == ESP_OK && !d->graphics_scan) {
            memcpy(d->mono_shadow,source,MONO_BYTES);
            d->mono_shadow_valid = true;
        }
    }
    xSemaphoreGive(d->mutex);
    return err;
}

static esp_err_t scan_start(solar_os_it8951_t *d, bool graphics)
{
    /* A fresh worker needs a complete baseline even after synchronous frames. */
    d->shadow_valid = false;
    d->graphics_scan = graphics;
    if (graphics) {
        d->gray_pending = solar_os_memory_alloc(GRAY_BYTES,SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,"it8951.gray-pending");
        d->pending_rows = solar_os_memory_calloc(HEIGHT,sizeof(*d->pending_rows),
            SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,"it8951.pending-rows");
        if (!d->gray_pending || !d->pending_rows) {
            solar_os_memory_free(d->gray_pending); d->gray_pending = NULL;
            solar_os_memory_free(d->pending_rows); d->pending_rows = NULL;
            return ESP_ERR_NO_MEM;
        }
        memcpy(d->gray_pending,d->gray,GRAY_BYTES);
        d->pending_x0 = WIDTH; d->pending_x1 = 0;
        d->pending_y0 = HEIGHT; d->pending_y1 = 0;
    } else {
        d->mono_shadow = solar_os_memory_alloc(MONO_BYTES,SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,"it8951.mono-shadow");
        if (!d->mono_shadow) return ESP_ERR_NO_MEM;
    }
    /* Graphics snapshots live in gray/gray_pending. The common worker only
     * queues generation notifications, avoiding two more full gray copies. */
    const esp_err_t err = epd_scan_worker_init(&d->scan_worker,
        graphics ? sizeof(d->frame_generation) : MONO_BYTES,scan_process,d);
    if (err == ESP_OK) scan_enable(d,true);
    else {
        solar_os_memory_free(d->mono_shadow); d->mono_shadow = NULL;
        solar_os_memory_free(d->gray_pending); d->gray_pending = NULL;
        solar_os_memory_free(d->pending_rows); d->pending_rows = NULL;
    }
    return err;
}

esp_err_t solar_os_it8951_present_sync(solar_os_it8951_t *d)
{
    if (!d || !d->spi || !d->mono) return ESP_ERR_INVALID_STATE;
    scan_stop(d);
    xSemaphoreTake(d->mutex, portMAX_DELAY);
    esp_err_t err = d->suspended ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (err == ESP_OK) {
        const dirty_area_t dirty = mono_to_gray(d,d->mono);
        err = finish_refresh(d,true,&dirty);
    }
    xSemaphoreGive(d->mutex);
    return err;
}

esp_err_t solar_os_it8951_present(solar_os_it8951_t *d)
{
    if (!d || !d->spi || !d->mono) return ESP_ERR_INVALID_STATE;
    if (!d->graphics_scan && __atomic_load_n(&d->scan_submit_ready,__ATOMIC_ACQUIRE) &&
        epd_scan_worker_submit(&d->scan_worker,d->mono) == ESP_OK) return ESP_OK;
    scan_stop(d);
    xSemaphoreTake(d->mutex,portMAX_DELAY);
    esp_err_t err = d->suspended ? ESP_ERR_INVALID_STATE :
        scan_start(d,false);
    xSemaphoreGive(d->mutex);
    if (err == ESP_OK) return epd_scan_worker_submit(&d->scan_worker,d->mono);
    if (err == ESP_ERR_NO_MEM) return solar_os_it8951_present_sync(d);
    return err;
}

esp_err_t solar_os_it8951_set_power_save(solar_os_it8951_t *d, bool sleep)
{
    if (!d || !d->spi) return ESP_ERR_INVALID_STATE;
    scan_stop(d);
    xSemaphoreTake(d->mutex, portMAX_DELAY);
    d->suspended = sleep;
    esp_err_t err = ESP_OK;
    if (sleep) {
        if (d->ready) {
            err = wait_lut(d);
            if (err == ESP_OK) err = command(d, 0x0003, NULL, 0);
        }
        if (err == ESP_OK) err = rails(d, false);
    } else if (!d->ready) err = controller_init(d);
    d->last_error = err;
    xSemaphoreGive(d->mutex);
    return err;
}

static uint8_t display_cb(u8x8_t *u8x8, uint8_t message, uint8_t arg, void *ptr)
{
    (void)ptr;
    if (message == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(u8x8, &display_info);
        return 1;
    }
    solar_os_it8951_t *d = (void *)((uint8_t *)u8x8 -
        offsetof(solar_os_it8951_t, u8g2) - offsetof(u8g2_t, u8x8));
    if (message == U8X8_MSG_DISPLAY_DRAW_TILE) return 1;
    if (message == U8X8_MSG_DISPLAY_REFRESH) return solar_os_it8951_present(d) == ESP_OK;
    if (message == U8X8_MSG_DISPLAY_SET_POWER_SAVE)
        return solar_os_it8951_set_power_save(d, arg != 0) == ESP_OK;
    if (message == U8X8_MSG_DISPLAY_INIT)
        return solar_os_it8951_set_power_save(d, false) == ESP_OK;
    return 0;
}

esp_err_t solar_os_it8951_init(solar_os_it8951_t *d, const solar_os_it8951_config_t *cfg)
{
    if (!d || !cfg || !cfg->spi_bus || !cfg->spi_bus[0] || cfg->rotation > 3 ||
        cfg->clock_hz < 100000U || cfg->clock_hz > 10000000U || cfg->vcom_mv != 1400)
        return ESP_ERR_INVALID_ARG;
    const int pins[] = {cfg->cs, cfg->reset, cfg->busy, cfg->power, cfg->ite_power};
    for (unsigned i = 0; i < 5; i++) {
        if (!GPIO_IS_VALID_GPIO(pins[i]) || (i != 2 && !GPIO_IS_VALID_OUTPUT_GPIO(pins[i])))
            return ESP_ERR_INVALID_ARG;
        for (unsigned j = i + 1; j < 5; j++) if (pins[i] == pins[j]) return ESP_ERR_INVALID_ARG;
    }
    memset(d, 0, sizeof(*d));
    d->config = *cfg;
    d->gray_levels = 4;
    d->temperature_c = 25;
    /* No caller-owned strings are retained after initialization. */
    d->config.spi_bus = NULL;
    d->mutex = xSemaphoreCreateMutexStatic(&d->mutex_storage);
    d->frame_mutex = xSemaphoreCreateMutexStatic(&d->frame_mutex_storage);
    if (!d->mutex || !d->frame_mutex) return ESP_ERR_NO_MEM;
    const gpio_config_t outputs = {.pin_bit_mask = (1ULL << cfg->cs) | (1ULL << cfg->reset) |
        (1ULL << cfg->power) | (1ULL << cfg->ite_power), .mode = GPIO_MODE_OUTPUT};
    TRY(gpio_config(&outputs));
    TRY(gpio_set_level(cfg->cs, 1));
    const gpio_config_t busy = {.pin_bit_mask = 1ULL << cfg->busy,
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    TRY(gpio_config(&busy));
    /* Enforce the ED103TC2 profile's 10 MHz ceiling after SDK rounding too. */
    const spi_device_interface_config_t spi_cfg = {.clock_speed_hz = (int)cfg->clock_hz,
        .mode = 0, .spics_io_num = -1, .queue_size = 1};
    esp_err_t err = solar_os_bus_spi_add_device(cfg->spi_bus, &spi_cfg, &d->spi);
    if (err != ESP_OK) return err;
    int actual_khz = 0;
    err = spi_device_get_actual_freq(d->spi, &actual_khz);
    if (err == ESP_OK && (actual_khz <= 0 || actual_khz > 10000))
        err = ESP_ERR_NOT_SUPPORTED;
    if (err != ESP_OK) { solar_os_it8951_deinit(d); return err; }
    d->spi_clock_khz = (unsigned)actual_khz;
    d->mono = solar_os_memory_calloc(1, MONO_BYTES, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "it8951.mono");
    d->gray = solar_os_memory_alloc(GRAY_BYTES, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "it8951.gray");
    solar_os_bus_info_t bus;
    d->transfer_size = WIDTH;
    if (solar_os_bus_find(cfg->spi_bus,SOLAR_OS_BUS_PROTOCOL_SPI,&bus)) {
        const unsigned limit = bus.config.spi.max_transfer_size;
        d->transfer_size = (limit < TRANSFER_BYTES ? limit : TRANSFER_BYTES) & ~3U;
    }
    if (d->transfer_size < WIDTH) err = ESP_ERR_INVALID_SIZE;
    d->transfer = solar_os_memory_alloc(d->transfer_size, SOLAR_OS_MEMORY_DMA, "it8951.spi");
    d->dirty_rows = solar_os_memory_calloc(HEIGHT,sizeof(*d->dirty_rows)+1U,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,"it8951.dirty-rows");
    if (d->dirty_rows) d->row_bpp = (uint8_t *)(d->dirty_rows+HEIGHT);
    if (!d->mono || !d->gray || !d->transfer || !d->dirty_rows) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        /* SolarOS's native monochrome convention is one=white, zero=black.
         * Raster MONO1 input uses its separate, explicit palette convention. */
        memset(d->mono, 0xff, MONO_BYTES);
        memset(d->gray, 0xff, GRAY_BYTES);
        static const u8g2_cb_t *const rotations[] = {U8G2_R0, U8G2_R1, U8G2_R2, U8G2_R3};
        u8g2_SetupDisplay(&d->u8g2, display_cb, u8x8_dummy_cb, u8x8_dummy_cb, u8x8_dummy_cb);
        u8g2_SetupBuffer(&d->u8g2, d->mono, (HEIGHT + 7U) / 8U,
            u8g2_ll_hvline_vertical_top_lsb, rotations[cfg->rotation]);
        u8g2_InitDisplay(&d->u8g2);
        err = d->last_error;
    }
    if (err == ESP_OK) {
        err = scan_start(d,false);
    }
    if (err != ESP_OK) solar_os_it8951_deinit(d);
    return err;
}

void solar_os_it8951_deinit(solar_os_it8951_t *d)
{
    if (!d) return;
    scan_stop(d);
    if (d->powered && d->spi) (void)solar_os_it8951_set_power_save(d, true);
    if (d->powered) (void)rails(d, false);
    if (d->spi) spi_bus_remove_device(d->spi);
    solar_os_memory_free(d->mono); solar_os_memory_free(d->gray);
    solar_os_memory_free(d->transfer);
    solar_os_memory_free(d->dirty_rows);
    memset(d, 0, sizeof(*d));
}

const char *solar_os_it8951_mode(const solar_os_it8951_t *d)
{
    if (!d) return "refresh=auto grayscale=4";
    solar_os_it8951_t *state = (solar_os_it8951_t *)d;
    xSemaphoreTake(state->mutex, portMAX_DELAY);
    const epd_scan_stats_t stats = epd_scan_worker_stats(&state->scan_worker);
    snprintf(state->mode_description, sizeof(state->mode_description),
        "refresh=%s grayscale=%u spi=%ukHz last=%ums upload=%ums lut=%ums area=%u,%u,%ux%u bpp=%u "
        "temp=%dC scan=%s submitted=%lu coalesced=%lu frames=%lu scan-last=%lldus "
        "fw=%s lut-id=%s bytes=%u chunks=%u regions=%u",
        d->full_refresh ? "full" : "auto", d->gray_levels, d->spi_clock_khz, d->last_refresh_ms, d->last_upload_ms,
        d->last_lut_ms, d->last_x, d->last_y, d->last_width, d->last_height, d->last_bpp,
        (int)d->temperature_c,d->scan_worker.task ? "async" : "sync",
        (unsigned long)stats.submitted,(unsigned long)stats.coalesced,
        (unsigned long)stats.frames,(long long)stats.last_scan_us,
        d->firmware_version,d->waveform_version,
        d->last_upload_bytes,d->last_chunks,d->last_regions);
    solar_os_epd_append_maintenance(&d->maintenance, state->mode_description, sizeof(state->mode_description));
    xSemaphoreGive(state->mutex);
    return state->mode_description;
}

esp_err_t solar_os_it8951_set_mode(solar_os_it8951_t *d, const char *mode)
{
    if (!d || !d->spi || !mode) return ESP_ERR_INVALID_ARG;
    if (!strncmp(mode, "cleanup-interval=", 17)) {
        xSemaphoreTake(d->mutex, portMAX_DELAY);
        const esp_err_t err = solar_os_epd_set_maintenance(&d->maintenance, mode);
        xSemaphoreGive(d->mutex);
        return err;
    }
    const bool full = !strcmp(mode, "refresh=full");
    const unsigned levels = !strcmp(mode,"grayscale=4") ? 4U :
        !strcmp(mode,"grayscale=16") ? 16U : 0U;
    if (!levels && !full && strcmp(mode, "refresh=auto")) return ESP_ERR_INVALID_ARG;
    scan_stop(d);
    xSemaphoreTake(d->mutex, portMAX_DELAY);
    if (levels) {
        if (d->gray_levels != levels) d->shadow_valid = false;
        d->gray_levels = levels;
    } else d->full_refresh = full;
    xSemaphoreGive(d->mutex);
    return ESP_OK;
}

static void logical_to_native(const u8g2_t *u8g2, unsigned x, unsigned y,
                               unsigned *nx, unsigned *ny)
{
    if (u8g2->cb == U8G2_R1) { *nx = WIDTH - 1U - y; *ny = x; }
    else if (u8g2->cb == U8G2_R2) { *nx = WIDTH - 1U - x; *ny = HEIGHT - 1U - y; }
    else if (u8g2->cb == U8G2_R3) { *nx = y; *ny = HEIGHT - 1U - x; }
    else { *nx = x; *ny = y; }
}

static uint8_t rgb_gray(uint16_t rgb)
{
    const unsigned r = ((rgb >> 11) & 31U) * 255U / 31U;
    const unsigned g = ((rgb >> 5) & 63U) * 255U / 63U;
    const unsigned b = (rgb & 31U) * 255U / 31U;
    return (uint8_t)(((r * 77U + g * 150U + b * 29U) * 15U + 32640U) / 65280U);
}

/* Pack a source row once, then reuse it for repeated destination rows. The
 * common unrotated scaled canvas otherwise quantizes and writes each native
 * pixel separately, including multiple read/modify/writes of one mono byte. */
static dirty_area_t compose_index8_rows(solar_os_it8951_t *d,
    const solar_os_display_raster_t *f, const uint8_t *palette)
{
    uint8_t packed[WIDTH/2U];
    uint8_t *image = d->gray_pending ? d->gray_pending : d->gray;
    dirty_area_t dirty = {WIDTH,0,HEIGHT,0,d->pending_rows ? d->pending_rows : d->dirty_rows};
    const unsigned bytes = f->width/2U;
    for (unsigned y = 0; y < f->height;) {
        const unsigned sy = (uint32_t)y*f->source_height/f->height;
        const uint8_t *row = f->data+(size_t)sy*f->source_stride;
        unsigned end = y+1U;
        while (end < f->height && (uint32_t)end*f->source_height/f->height == sy) ++end;
        for (unsigned x = 0; x < f->width; x += 2U) {
            const unsigned sx0 = (uint32_t)x*f->source_width/f->width;
            const unsigned sx1 = (uint32_t)(x+1U)*f->source_width/f->width;
            packed[x/2U] = (uint8_t)((palette[row[sx0]] << 4) | palette[row[sx1]]);
        }
        for (unsigned yy = y; yy < end; yy++) {
            uint8_t *out = image+(size_t)(f->y+yy)*(WIDTH/2U)+f->x/2U;
            if (!memcmp(out,packed,bytes)) continue;
            unsigned first = 0, last = bytes;
            while (out[first] == packed[first]) ++first;
            while (out[last-1U] == packed[last-1U]) --last;
            while (first < last) {
                unsigned count = ((WIDTH-f->x-first*2U) & 31U)/2U;
                if (!count) count = 16U;
                if (count > last-first) count = last-first;
                if (memcmp(out+first,packed+first,count)) {
                    memcpy(out+first,packed+first,count);
                    dirty_include(&dirty,f->x+first*2U,f->y+yy,count*2U);
                }
                first += count;
            }
        }
        /* Set all repeated rows in a mono tile with one write per column. */
        for (unsigned yy = y; yy < end;) {
            const unsigned ny = f->y+yy, shift = ny & 7U;
            const unsigned count = end-yy < 8U-shift ? end-yy : 8U-shift;
            const uint8_t mask = (uint8_t)(((1U << count)-1U) << shift);
            uint8_t *mono = d->mono+(size_t)(ny/8U)*WIDTH+f->x;
            for (unsigned x = 0; x < f->width; x += 2U) {
                const uint8_t pair = packed[x/2U];
                mono[x] = (mono[x] & (uint8_t)~mask) | (pair & 0x80U ? mask : 0);
                mono[x+1U] = (mono[x+1U] & (uint8_t)~mask) | (pair & 8U ? mask : 0);
            }
            yy += count;
        }
        y = end;
    }
    return dirty;
}

esp_err_t solar_os_it8951_present_frame(solar_os_it8951_t *d,
                                       const solar_os_display_raster_t *f)
{
    if (!d || !d->spi || !f || !f->data || !f->width || !f->height ||
        !f->source_width || !f->source_height) return ESP_ERR_INVALID_ARG;
    size_t row_bytes;
    switch (f->format) {
    case SOLAR_OS_DISPLAY_FORMAT_MONO1: row_bytes = (f->source_width + 7U) / 8U; break;
    case SOLAR_OS_DISPLAY_FORMAT_INDEX2: row_bytes = (f->source_width + 3U) / 4U; break;
    case SOLAR_OS_DISPLAY_FORMAT_INDEX8: row_bytes = f->source_width; break;
    case SOLAR_OS_DISPLAY_FORMAT_RGB565: row_bytes = (size_t)f->source_width * 2U; break;
    default: return ESP_ERR_NOT_SUPPORTED;
    }
    if (f->source_stride < row_bytes || f->data_size <
        (size_t)(f->source_height - 1U) * f->source_stride + row_bytes ||
        (size_t)f->x + f->width > u8g2_GetDisplayWidth(&d->u8g2) ||
        (size_t)f->y + f->height > u8g2_GetDisplayHeight(&d->u8g2)) return ESP_ERR_INVALID_SIZE;
    const bool indexed = f->format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 ||
                         f->format == SOLAR_OS_DISPLAY_FORMAT_INDEX2;
    uint8_t palette_gray[256];
    if (indexed) {
        if (!f->palette_rgb565 || !f->palette_size || f->palette_size > 256U ||
            (f->format == SOLAR_OS_DISPLAY_FORMAT_INDEX2 && f->palette_size > 4U) ||
            (f->clear_background && f->background_index >= f->palette_size))
            return ESP_ERR_INVALID_ARG;
        /* Quantize each palette entry once, rather than every scaled pixel. */
        for (unsigned i = 0; i < f->palette_size; i++)
            palette_gray[i] = rgb_gray(f->palette_rgb565[i]);
        /* Validate before changing the destination, including off-screen source pixels. */
        for (unsigned y = 0; y < f->source_height; y++) {
            const uint8_t *row = f->data + (size_t)y * f->source_stride;
            for (unsigned x = 0; x < f->source_width; x++) {
                const unsigned index = f->format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 ? row[x] :
                    (row[x / 4U] >> (2U * (x & 3U))) & 3U;
                if (index >= f->palette_size) return ESP_ERR_INVALID_ARG;
            }
        }
    }
    esp_err_t err = ESP_OK;
    if (!d->graphics_scan || !__atomic_load_n(&d->scan_submit_ready,__ATOMIC_ACQUIRE)) {
        scan_stop(d);
        xSemaphoreTake(d->mutex,portMAX_DELAY);
        err = d->suspended ? ESP_ERR_INVALID_STATE : scan_start(d,true);
        xSemaphoreGive(d->mutex);
        /* Allocation failure retains synchronous raster support. */
        if (err == ESP_ERR_NO_MEM) { scan_stop(d); err = ESP_OK; }
    }
    const bool async = d->gray_pending != NULL;
    SemaphoreHandle_t lock = async ? d->frame_mutex : d->mutex;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (err != ESP_OK) {
        xSemaphoreGive(lock);
        return err;
    }
    dirty_area_t dirty = {WIDTH,0,HEIGHT,0,async ? d->pending_rows : d->dirty_rows};
    if (!async) memset(d->dirty_rows,0,HEIGHT*sizeof(*d->dirty_rows));
    if (f->clear_background) {
        dirty.x0 = dirty.y0 = 0; dirty.x1 = WIDTH; dirty.y1 = HEIGHT;
        for (unsigned y = 0; y < HEIGHT; y++) dirty.rows[y] =
            (UINT64_C(1) << ((WIDTH+31U)/32U))-1U;
        const uint8_t background = indexed ? palette_gray[f->background_index] :
            f->format == SOLAR_OS_DISPLAY_FORMAT_RGB565 ? rgb_gray(f->background_rgb565) :
            ((!!f->background_index != f->palette_inverted) ? 0 : 15);
        memset(async ? d->gray_pending : d->gray, (background << 4) | background, GRAY_BYTES);
        memset(d->mono, background >= 8 ? 0xff : 0, MONO_BYTES);
    }
    if (f->format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 && d->u8g2.cb == U8G2_R0 &&
        !(f->x & 1U) && !(f->width & 1U)) {
        const dirty_area_t changed = compose_index8_rows(d,f,palette_gray);
        if (changed.x1) {
            if (changed.x0 < dirty.x0) dirty.x0 = changed.x0;
            if (changed.x1 > dirty.x1) dirty.x1 = changed.x1;
            if (changed.y0 < dirty.y0) dirty.y0 = changed.y0;
            if (changed.y1 > dirty.y1) dirty.y1 = changed.y1;
        }
    } else for (unsigned y = 0; y < f->height; y++) {
        const unsigned sy = (uint32_t)y * f->source_height / f->height;
        const uint8_t *row = f->data + (size_t)sy * f->source_stride;
        for (unsigned x = 0; x < f->width; x++) {
            const unsigned sx = (uint32_t)x * f->source_width / f->width;
            uint8_t gray;
            if (f->format == SOLAR_OS_DISPLAY_FORMAT_MONO1) {
                const bool black = ((row[sx / 8U] >> (sx & 7U)) & 1U) != f->palette_inverted;
                gray = black ? 0 : 15;
            } else if (f->format == SOLAR_OS_DISPLAY_FORMAT_RGB565) {
                gray = rgb_gray((uint16_t)((uint16_t)row[sx * 2U] << 8) | row[sx * 2U + 1U]);
            } else {
                const unsigned index = f->format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 ? row[sx] :
                    (row[sx / 4U] >> (2U * (sx & 3U))) & 3U;
                gray = palette_gray[index];
            }
            unsigned nx, ny;
            logical_to_native(&d->u8g2, f->x + x, f->y + y, &nx, &ny);
            if (set_pixel(d,nx,ny,gray)) dirty_include(&dirty,nx,ny,1);
        }
    }
    if (async) {
        /* Accumulate all patches until the worker takes the target. Replacing
         * the notification must never lose an earlier disjoint rectangle. */
        if (dirty.x1) {
            if (dirty.x0 < d->pending_x0) d->pending_x0 = dirty.x0;
            if (dirty.x1 > d->pending_x1) d->pending_x1 = dirty.x1;
            if (dirty.y0 < d->pending_y0) d->pending_y0 = dirty.y0;
            if (dirty.y1 > d->pending_y1) d->pending_y1 = dirty.y1;
        }
        ++d->frame_generation;
        err = epd_scan_worker_submit(&d->scan_worker,(const uint8_t *)&d->frame_generation);
    } else err = finish_refresh(d,false,&dirty);
    xSemaphoreGive(lock);
    return err;
}

esp_err_t solar_os_it8951_present_surface(solar_os_it8951_t *d,
                                         const solar_os_display_surface_t *s)
{
    if (!s || !d || s->format != SOLAR_OS_DISPLAY_FORMAT_INDEX8 ||
        s->native_width != WIDTH || s->native_height != HEIGHT ||
        s->width != u8g2_GetDisplayWidth(&d->u8g2) ||
        s->height != u8g2_GetDisplayHeight(&d->u8g2)) return ESP_ERR_INVALID_ARG;
    const solar_os_display_raster_t f = {.data = s->data, .data_size = s->data_size,
        .palette_rgb565 = s->palette_rgb565, .palette_size = s->palette_size,
        .source_width = s->width, .source_height = s->height, .source_stride = s->stride,
        .width = s->width, .height = s->height, .format = s->format};
    return solar_os_it8951_present_frame(d, &f);
}

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/drivers/epd_it8951.h"

/* Detect a producer accidentally taking a lock held by the scan callback. */
static SemaphoreHandle_t test_mutex_create(StaticSemaphore_t *s) { *s=1; return s; }
static int test_mutex_take(SemaphoreHandle_t s, unsigned timeout)
{ (void)timeout; assert(s && *s==1); *s=0; return 1; }
static int test_mutex_give(SemaphoreHandle_t s)
{ assert(s && *s==0); *s=1; return 1; }
#define xSemaphoreCreateMutexStatic test_mutex_create
#define xSemaphoreTake test_mutex_take
#define xSemaphoreGive test_mutex_give
#include "../../src/drivers/epd_it8951.c"

typedef union { max_align_t alignment; size_t bytes; } test_allocation_t;
static size_t allocated_bytes, fail_allocation_size;
void *solar_os_memory_alloc(size_t bytes, solar_os_memory_class_t kind, const char *tag)
{
    (void)kind; assert(tag);
    if (bytes == fail_allocation_size) { fail_allocation_size = 0; return NULL; }
    test_allocation_t *allocation = malloc(sizeof(*allocation)+bytes);
    if (!allocation) return NULL;
    allocation->bytes = bytes;
    allocated_bytes += bytes;
    return allocation+1;
}
void *solar_os_memory_calloc(size_t count, size_t bytes, solar_os_memory_class_t kind, const char *tag)
{
    void *allocation = solar_os_memory_alloc(count*bytes,kind,tag);
    if (allocation) memset(allocation,0,count*bytes);
    return allocation;
}
void solar_os_memory_free(void *pointer)
{
    if (!pointer) return;
    test_allocation_t *allocation = (test_allocation_t *)pointer-1;
    assert(allocated_bytes >= allocation->bytes);
    allocated_bytes -= allocation->bytes;
    free(allocation);
}
static size_t allocation_bytes(const void *pointer)
{
    return ((const test_allocation_t *)pointer-1)->bytes;
}

static int64_t now;
static bool external_dma;
static unsigned direct_calls;
static unsigned delay_calls;
static int64_t busy_until;
bool esp_ptr_external_ram(const void *pointer) { return pointer && external_dma; }
bool esp_ptr_dma_ext_capable(const void *pointer) { return pointer && external_dma; }
void esp_rom_delay_us(uint32_t us) { now += us; }
static bool held, selected, stuck_ready, stuck_lut, wrong_panel;
static unsigned phase, preamble, current_command, arg_count, last_reg;
static unsigned updates, last_waveform, rows, removed;
static unsigned image_x, image_y, image_width, image_height, image_format;
static size_t upload_bytes;
static size_t image_received;
static bool upload_cycle;
static unsigned loaded_x0,loaded_x1,loaded_y0,loaded_y1;
static unsigned upload_calls,interrupt_calls,load_calls;
static unsigned spi_transfer_limit = 4096;
static int fail_bulk_after = -1;
static int fail_after = -1;
static int fail_power_pin = -1;
static uint16_t registers[0x1300];
static uint16_t args[8];
static uint8_t uploaded[(size_t)WIDTH * HEIGHT];
static unsigned levels[49];
static bool sleep_retained[49];
static unsigned sleep_power_losses;
static bool worker_init_fail;
static int16_t temperature_sample = 27;
static esp_err_t temperature_error;
static unsigned temperature_writes, commanded_temperature;
static int spi_clock_khz;
static int last_write_clock_khz, last_read_clock_khz;
static bool overspeed;
static unsigned info_words;
static bool info_word_pending;
static const uint8_t info_firmware[16] = "Seeed_v.0.1";
static const uint8_t info_waveform[16] = "3M29T";
static void (*during_upload)(void);

static esp_err_t sample_temperature(int16_t *value)
{
    *value = temperature_sample;
    return temperature_error;
}

esp_err_t epd_scan_worker_init(epd_scan_worker_t *w, size_t n, epd_scan_process_fn fn, void *ctx)
{
    if (worker_init_fail) return ESP_ERR_NO_MEM;
    memset(w,0,sizeof(*w));
    w->latest = solar_os_memory_alloc(n,SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,"worker.latest");
    w->snapshot = solar_os_memory_alloc(n,SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,"worker.snapshot");
    assert(w->latest && w->snapshot);
    w->lock = xSemaphoreCreateMutexStatic(&w->lock_storage);
    w->task = (void *)1; w->frame_size = n; w->process = fn; w->context = ctx;
    return ESP_OK;
}
void epd_scan_worker_enable(epd_scan_worker_t *w, bool enabled)
{
    w->enabled = enabled;
    if (!enabled) w->has_target = false;
}
esp_err_t epd_scan_worker_submit(epd_scan_worker_t *w, const uint8_t *source)
{
    assert(w->task && w->enabled);
    if (w->has_target) w->stats.coalesced++;
    memcpy(w->latest,source,w->frame_size); w->has_target = true; w->stats.submitted++;
    return ESP_OK;
}
epd_scan_stats_t epd_scan_worker_stats(epd_scan_worker_t *w) { return w->stats; }
void epd_scan_worker_deinit(epd_scan_worker_t *w)
{
    solar_os_memory_free(w->latest); solar_os_memory_free(w->snapshot);
    w->latest = w->snapshot = NULL; w->task = NULL; w->has_target = false;
}
static void pump(epd_scan_worker_t *w)
{
    assert(w->task && w->enabled && w->has_target);
    memcpy(w->snapshot,w->latest,w->frame_size); w->has_target = false;
    bool pending,scanned;
    const unsigned before=updates;
    assert(w->process(w->context,w->snapshot,true,&pending,&scanned) == ESP_OK);
    assert(!pending && scanned == (updates != before));
    ++w->stats.frames;
    if (scanned) ++w->stats.scans;
}

static esp_err_t present_frame_and_wait(solar_os_it8951_t *d,
    const solar_os_display_raster_t *f)
{
    const esp_err_t err = solar_os_it8951_present_frame(d,f);
    if (err == ESP_OK && d->scan_worker.task && d->scan_worker.has_target) pump(&d->scan_worker);
    return err;
}

int64_t esp_timer_get_time(void) { return now; }
void vTaskDelay(TickType_t ticks)
{
    assert(ticks);
    delay_calls++;
    /* Automatic sleep isolates unretained GPIOs while the worker yields.
     * Losing either supply or the deasserted reset interrupts the controller. */
    if (levels[11] && levels[21] &&
        (!sleep_retained[11] || !sleep_retained[21] ||
         (levels[12] && !sleep_retained[12]))) ++sleep_power_losses;
    now += (int64_t)ticks * 1000;
}
esp_err_t gpio_config(const gpio_config_t *cfg) { assert(cfg); return ESP_OK; }
esp_err_t gpio_sleep_sel_dis(gpio_num_t pin)
{
    assert(pin >= 0 && pin < 49);
    sleep_retained[pin] = true;
    return ESP_OK;
}
int gpio_get_level(gpio_num_t pin)
{
    assert(pin == 13);
    if (stuck_ready) return 0;
    if (now < busy_until) return 0;
    if (info_word_pending) { info_word_pending = false; return 0; }
    return 1;
}
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    assert(pin >= 0 && pin < 49);
    if (pin == fail_power_pin) { fail_power_pin = -1; return ESP_FAIL; }
    levels[pin] = level;
    if (pin == 10) {
        if (!level) { assert(held && !selected); selected = true; phase = 0; }
        else { selected = false; info_word_pending = false; }
    }
    return ESP_OK;
}
esp_err_t solar_os_bus_spi_add_device(const char *bus,
    const spi_device_interface_config_t *cfg, spi_device_handle_t *spi)
{
    assert(!strcmp(bus, "spi0") && cfg->spics_io_num == -1 && cfg->mode == 0);
    assert(cfg->clock_speed_hz == 5000000 || cfg->clock_speed_hz == 10000000);
    spi_clock_khz = cfg->clock_speed_hz / 1000;
    *spi = (void *)1;
    return ESP_OK;
}
bool solar_os_bus_find(const char *name, solar_os_bus_protocol_t protocol,
    solar_os_bus_info_t *info)
{
    assert(!strcmp(name,"spi0") && protocol == SOLAR_OS_BUS_PROTOCOL_SPI);
    *info = (solar_os_bus_info_t){0};
    info->config.spi.max_transfer_size = spi_transfer_limit;
    return true;
}
esp_err_t spi_bus_remove_device(spi_device_handle_t spi) { assert(spi); removed++; return ESP_OK; }
esp_err_t spi_device_get_actual_freq(spi_device_handle_t spi, int *freq_khz)
{ assert(spi && freq_khz); *freq_khz = overspeed ? 26666 : spi_clock_khz; return ESP_OK; }
esp_err_t spi_device_acquire_bus(spi_device_handle_t spi, unsigned wait)
{ assert(spi && !held && wait == portMAX_DELAY); held = true; return ESP_OK; }
void spi_device_release_bus(spi_device_handle_t spi)
{ assert(spi && held && !selected); held = false; }

esp_err_t spi_device_polling_transmit(spi_device_handle_t spi, spi_transaction_t *t)
{
    assert(spi && selected && held);
    if (during_upload && preamble == 0 && t->length > 16U) {
        void (*hook)(void) = during_upload; during_upload = NULL; hook();
    }
    if (fail_after == 0) { fail_after = -1; return ESP_FAIL; }
    if (fail_after > 0) fail_after--;
    if (t->override_freq_hz) {
        /* Model SDK rounding and its device-local, retained clock override. */
        spi_clock_khz = t->override_freq_hz == 15000000U ? 16000 :
            (int)(t->override_freq_hz / 1000U);
    }
    const uint8_t *tx = t->tx_buffer;
    const size_t size = t->length / 8U;
    if (!phase++) {
        assert(size == 2 && tx);
        preamble = ((unsigned)tx[0] << 8) | tx[1];
        assert(preamble == 0 || preamble == 0x1000 || preamble == 0x6000);
        if (preamble == 0x1000) last_read_clock_khz = spi_clock_khz;
        else last_write_clock_khz = spi_clock_khz;
        return ESP_OK;
    }
    if (preamble == 0x1000) {
        if (phase == 2) { assert(size == 2 && !t->rx_buffer); return ESP_OK; }
        assert(phase >= 3 && t->rx_buffer);
        uint8_t *rx = t->rx_buffer;
        memset(rx, 0, size);
        if (current_command == 0x0302) {
            assert(size == 2 && info_words < 20 && !info_word_pending);
            const uint16_t info[] = {wrong_panel ? 800 : WIDTH, HEIGHT, 0x3456, 0x12};
            const uint8_t *identity = info_words < 12 ? info_firmware : info_waveform;
            const unsigned offset = (info_words < 12 ? info_words-4U : info_words-12U)*2U;
            const uint16_t value = info_words < 4 ? info[info_words] :
                (uint16_t)((uint16_t)identity[offset] << 8) | identity[offset+1U];
            rx[0] = value >> 8; rx[1] = value;
            info_word_pending = ++info_words < 20;
        } else {
            assert(current_command == 0x0010 && size == 2);
            const uint16_t value = last_reg == 0x1224 ? (unsigned)stuck_lut : registers[last_reg];
            rx[0] = value >> 8; rx[1] = value;
        }
    } else if (preamble == 0x6000) {
        assert(size == 2 && phase == 2);
        current_command = ((unsigned)tx[0] << 8) | tx[1]; arg_count = 0;
        if (current_command == 0x0302) info_words = 0;
        if (current_command == 0x0001) upload_cycle = false;
        if (current_command == 0x0022) {
            const unsigned bpp = image_format == 0x0030 ? 1U : image_format == 0x0100 ? 2U : 4U;
            const unsigned width = bpp == 1 ? image_width*8U : image_width;
            assert(image_received == (size_t)width*image_height*bpp/8U);
        }
    } else if (current_command == 0x0021 && arg_count == 5) {
        if (fail_bulk_after == 0) { fail_bulk_after=-1; return ESP_FAIL; }
        if (fail_bulk_after > 0) --fail_bulk_after;
        const unsigned bpp = image_format == 0x0030 ? 1U : image_format == 0x0100 ? 2U : 4U;
        const unsigned width = bpp == 1 ? image_width*8U : image_width;
        const unsigned x0 = bpp == 1 ? image_x*8U : image_x;
        const size_t stride = width*bpp/8U;
        assert(size && size <= spi_transfer_limit);
        assert(image_received+size <= stride*image_height);
        for (size_t i = 0; i < size; i++) {
            const unsigned y = (image_received+i)/stride;
            const unsigned byte = (image_received+i)%stride;
            for (unsigned p = 0; p < 8U/bpp; p++) {
                unsigned x,gray;
                if (bpp == 1) {
                    x = (byte ^ 1U)*8U+p;
                    gray = (tx[i] & (1U << p)) ? 255 : 0;
                } else {
                    x = byte*(8U/bpp)+p;
                    const unsigned sample = (tx[i] >> (8U-bpp-p*bpp)) & ((1U << bpp)-1U);
                    /* Datasheet Fig. 7-16 zero-pads 2-bit samples to the
                     * high bits: max 3 becomes level 12, not white 15. */
                    gray = sample*(bpp == 2 ? 4U : 1U)*17U;
                }
                uploaded[(size_t)(image_y+y)*WIDTH+x0+x] = gray;
            }
        }
        image_received += size; upload_bytes += size;
        rows = image_received/stride;
        ++upload_calls;
    } else {
        assert(size == 2 && arg_count < 8);
        args[arg_count++] = (uint16_t)(((unsigned)tx[0] << 8) | tx[1]);
        if (current_command == 0x0010) last_reg = args[0];
        if (current_command == 0x0011 && arg_count == 2) registers[args[0]] = args[1];
        if (current_command == 0x0021 && arg_count == 5) {
            if (!upload_cycle) {
                upload_bytes = upload_calls = interrupt_calls = load_calls = 0;
                loaded_x0 = WIDTH; loaded_x1 = 0; loaded_y0 = HEIGHT; loaded_y1 = 0;
                upload_cycle = true;
            }
            ++load_calls; rows = 0; image_received = 0;
            image_format = args[0]; image_x = args[1]; image_y = args[2];
            image_width = args[3]; image_height = args[4];
            assert(image_format == 0x0030 || image_format == 0x0100 || image_format == 0x0120);
            const unsigned x = image_format == 0x0030 ? image_x*8U : image_x;
            const unsigned w = image_format == 0x0030 ? image_width*8U : image_width;
            assert(!(x & 31U) && !(w & 15U) && w && image_height);
            assert(x + w <= WIDTH && image_y + image_height <= HEIGHT);
            if (x < loaded_x0) loaded_x0 = x;
            if (x+w > loaded_x1) loaded_x1 = x+w;
            if (image_y < loaded_y0) loaded_y0 = image_y;
            if (image_y+image_height > loaded_y1) loaded_y1 = image_y+image_height;
        }
        if (current_command == 0x0039 && arg_count == 2) assert(args[0] == 2 && args[1] == 1400);
        if (current_command == 0x0040 && arg_count == 2) {
            assert(args[0] == 1 && args[1] <= 50);
            temperature_writes++; commanded_temperature = args[1];
        }
        if (current_command == 0x0034 && arg_count == 5) {
            const bool binary = image_format == 0x0030;
            assert(rows == image_height && args[0] <= loaded_x0 && args[1] <= loaded_y0);
            assert(args[0]+args[2] >= loaded_x1 && args[1]+args[3] >= loaded_y1);
            assert(!!(registers[0x113a] & 4U) == binary);
            if (binary) assert(registers[0x1250] == 0x00ff);
            updates++; last_waveform = args[4];
            upload_cycle = false;
        }
    }
    return ESP_OK;
}

esp_err_t spi_device_transmit(spi_device_handle_t spi, spi_transaction_t *t)
{
    assert(current_command == 0x0021 && arg_count == 5 && t->length >= 64U*8U);
    if (t->flags & SPI_TRANS_DMA_USE_PSRAM) {
        assert(external_dma && !(((uintptr_t)t->tx_buffer | (t->length/8U)) & 63U));
        assert(t->flags & SPI_TRANS_DMA_BUFFER_ALIGN_MANUAL);
        direct_calls++;
    }
    const esp_err_t err = spi_device_polling_transmit(spi,t);
    ++interrupt_calls;
    return err;
}

static solar_os_it8951_config_t config(void)
{
    return (solar_os_it8951_config_t){.spi_bus="spi0", .cs=10, .reset=12, .busy=13,
        .power=11, .ite_power=21, .clock_hz=10000000, .rotation=0, .vcom_mv=1400,
        .read_temperature=sample_temperature};
}

static void test_upload_packing(solar_os_it8951_t *d)
{
    uint8_t row[WIDTH/2U];memset(row,0xff,sizeof(row));
    for (unsigned pair=0;pair<256U;pair++) {
        row[0]=(uint8_t)pair;
        const unsigned hi=pair >> 4,lo=pair & 15U;
        const unsigned expected=(hi==0 || hi==15) && (lo==0 || lo==15) ? 1U : 4U;
        assert(row_bpp(row)==expected);
    }
    for (unsigned bits = 0; bits < 256; bits++) {
        uint8_t source[4];
        for (unsigned i = 0; i < 4; i++) {
            source[i] = (bits & (0x80U >> (2U*i)) ? 0xf0 : 0) |
                (bits & (0x40U >> (2U*i)) ? 0x0f : 0);
        }
        assert(pack_binary_byte(source) == bits);
    }
    const dirty_area_t full = {0,WIDTH,0,HEIGHT,NULL};
    for (unsigned binary = 0; binary < 2; binary++) {
        uint32_t random = 42;
        for (size_t i = 0; i < GRAY_BYTES; i++) {
            random = random*1664525U + 1013904223U;
            d->gray[i] = binary ? ((random & 0x80000000U ? 0xf0 : 0) |
                (random & 0x40000000U ? 0x0f : 0)) : (uint8_t)(random >> 24);
        }
        assert(finish_refresh(d,binary,&full) == ESP_OK);
        assert(upload_bytes == (size_t)WIDTH*HEIGHT/(binary ? 8U : 2U));
        assert(load_calls == 1 && d->last_regions == 1);
        assert(upload_calls == d->last_chunks && upload_calls <= HEIGHT/4U);
        assert(interrupt_calls == upload_calls);
        for (unsigned y = 0; y < HEIGHT; y++)
            for (unsigned x = 0; x < WIDTH; x++)
                assert(uploaded[(size_t)y*WIDTH+x] == gray_at(d->gray,WIDTH-1U-x,y)*17U);
    }
}

static void test_sparse_uploads(solar_os_it8951_t *d)
{
    assert(solar_os_it8951_set_mode(d,"refresh=auto")==ESP_OK);
    uint8_t pixel = 1;
    const uint16_t palette[] = {0,0xffff};
    solar_os_display_raster_t f = {.data=&pixel,.data_size=1,
        .source_width=1,.source_height=1,.source_stride=1,.width=1,.height=1,
        .format=SOLAR_OS_DISPLAY_FORMAT_INDEX8,.palette_rgb565=palette,.palette_size=2,
        .clear_background=true,.background_index=1};
    assert(present_frame_and_wait(d,&f)==ESP_OK);
    const unsigned before = updates;
    f.clear_background=false; f.x=32;f.y=16;pixel=0;
    assert(solar_os_it8951_present_frame(d,&f)==ESP_OK);
    f.x=1840;f.y=1380;
    assert(solar_os_it8951_present_frame(d,&f)==ESP_OK);
    pump(&d->scan_worker);
    assert(updates==before+1 && last_waveform==1);
    assert(load_calls==2 && upload_bytes==8 && d->last_regions==2);
    assert(d->last_upload_bytes==8 && d->last_chunks==2 && interrupt_calls==0);
    assert(uploaded[(size_t)16*WIDTH+WIDTH-1U-32U]==0);
    assert(uploaded[(size_t)1380*WIDTH+WIDTH-1U-1840U]==0);
    assert(uploaded[(size_t)700*WIDTH+WIDTH/2U]==255);
    /* A queued edit that is replaced with the current image is a no-op. */
    pixel=1;assert(solar_os_it8951_present_frame(d,&f)==ESP_OK);
    pixel=0;assert(solar_os_it8951_present_frame(d,&f)==ESP_OK);
    pump(&d->scan_worker);
    assert(updates==before+1);
    /* Distant changes within one indexed row must also remain sparse. */
    uint8_t line[WIDTH];memset(line,1,sizeof(line));line[32]=line[1840]=0;
    f.data=line;f.data_size=sizeof(line);f.source_width=f.source_stride=f.width=WIDTH;
    f.x=0;f.y=200;
    assert(present_frame_and_wait(d,&f)==ESP_OK);
    assert(load_calls==2 && upload_bytes==8 && d->last_regions==2);
    /* Excessive fragmentation falls back to one bounded rectangle. */
    f.data=&pixel;f.data_size=1;f.source_width=f.source_stride=f.width=1;
    for (unsigned i=0;i<MAX_REGIONS+1U;i++) {
        f.x=32+(i%20U)*64;f.y=300+i*2U;
        assert(solar_os_it8951_present_frame(d,&f)==ESP_OK);
    }
    const unsigned before_fragmented = updates;
    pump(&d->scan_worker);
    assert(updates==before_fragmented+1 && load_calls==1 && d->last_regions==1);
    for (unsigned i=0;i<MAX_REGIONS+1U;i++)
        assert(uploaded[(size_t)(300+i*2U)*WIDTH+WIDTH-1U-(32+(i%20U)*64)]==0);
    /* Native U8g2 damage uses the same sparse upload plan. */
    memset(d->mono,0xff,MONO_BYTES);
    assert(solar_os_it8951_present_sync(d)==ESP_OK);
    d->mono[2U*WIDTH+32U]^=1U;
    d->mono[(1380U/8U)*WIDTH+1840U]^=(uint8_t)(1U << (1380U & 7U));
    assert(solar_os_it8951_present_sync(d)==ESP_OK);
    assert(load_calls==2 && upload_bytes==8 && last_waveform==1);
}

static void test_encoding_cache(solar_os_it8951_t *d)
{
    assert(solar_os_it8951_set_mode(d,"grayscale=16")==ESP_OK);
    const uint16_t palette[]={0x52aa,0x8410,0xffff};
    assert(rgb_gray(palette[0])==5 && rgb_gray(palette[1])==8);
    uint8_t pixel=2;
    solar_os_display_raster_t f={.data=&pixel,.data_size=1,
        .source_width=1,.source_height=1,.source_stride=1,.width=1,.height=1,
        .x=7,.y=7,.format=SOLAR_OS_DISPLAY_FORMAT_INDEX8,
        .palette_rgb565=palette,.palette_size=3,.clear_background=true,.background_index=2};
    assert(present_frame_and_wait(d,&f)==ESP_OK && d->last_bpp==1);
    f.clear_background=false;pixel=0;
    assert(present_frame_and_wait(d,&f)==ESP_OK && d->last_bpp==4);
    f.x=17;f.y=1300;pixel=1;
    assert(present_frame_and_wait(d,&f)==ESP_OK && d->last_bpp==4);
    pixel=2;
    assert(present_frame_and_wait(d,&f)==ESP_OK && d->last_bpp==4);
    f.x=7;f.y=7;
    assert(present_frame_and_wait(d,&f)==ESP_OK && d->last_bpp==1);
}

static void test_four_level_uploads(solar_os_it8951_t *d)
{
    const dirty_area_t full = {0,WIDTH,0,HEIGHT,NULL};
    assert(solar_os_it8951_set_mode(d,"grayscale=16")==ESP_OK);
    /* Four-level images still need 4-bit transport to reach true white. */
    for (size_t i=0;i<GRAY_BYTES;i++)
        d->gray[i]=(uint8_t)((((i*2U)%4U)*5U << 4) | (((i*2U+1U)%4U)*5U));
    assert(finish_refresh(d,false,&full)==ESP_OK);
    assert(d->last_bpp==4 && image_format==0x0120 && last_waveform==2);
    assert(upload_bytes==(size_t)WIDTH*HEIGHT/2U && interrupt_calls==upload_calls);
    for (unsigned y=0;y<HEIGHT;y++) for (unsigned x=0;x<WIDTH;x++)
        assert(uploaded[(size_t)y*WIDTH+x]==gray_at(d->gray,WIDTH-1U-x,y)*17U);
    /* Four-level output quantizes every shade; retained image stays exact. */
    for (size_t i=0;i<GRAY_BYTES;i++)
        d->gray[i]=(uint8_t)((((i*2U)%16U) << 4) | ((i*2U+1U)%16U));
    assert(solar_os_it8951_set_mode(d,"grayscale=4")==ESP_OK);
    assert(finish_refresh(d,false,&full)==ESP_OK && d->last_bpp==4);
    assert(image_format==0x0120 && uploaded[0]==255 && uploaded[WIDTH-1U]==0);
    assert(strstr(solar_os_it8951_mode(d),"grayscale=4"));
    for (unsigned y=0;y<HEIGHT;y++) for (unsigned x=0;x<WIDTH;x++)
        assert(uploaded[(size_t)y*WIDTH+x]==((gray_at(d->gray,WIDTH-1U-x,y)+2U)/5U)*85U);
    assert(solar_os_it8951_set_mode(d,"grayscale=8")==ESP_ERR_INVALID_ARG);
    assert(solar_os_it8951_set_mode(d,"grayscale=16")==ESP_OK);
    fail_bulk_after=1;
    assert(finish_refresh(d,false,&full)==ESP_FAIL);
    assert(!d->ready && !d->shadow_valid && !held && !selected);
    assert(finish_refresh(d,false,&full)==ESP_OK && d->last_bpp==4);
    assert(upload_bytes==(size_t)WIDTH*HEIGHT/2U);
    for (unsigned y=0;y<HEIGHT;y++) for (unsigned x=0;x<WIDTH;x++)
        assert(uploaded[(size_t)y*WIDTH+x]==gray_at(d->gray,WIDTH-1U-x,y)*17U);
}

static void test_indexed_palette(solar_os_it8951_t *d)
{
    uint16_t palette[256];
    uint8_t pixels[256];
    for (unsigned i = 0; i < 256; i++) {
        pixels[i] = i;
        palette[i] = (uint16_t)(i*257U);
    }
    const solar_os_display_raster_t f = {.data=pixels, .data_size=sizeof(pixels),
        .source_width=256, .source_height=1, .source_stride=256,
        .x=7, .y=9, .width=512, .height=2, .format=SOLAR_OS_DISPLAY_FORMAT_INDEX8,
        .palette_rgb565=palette, .palette_size=256,
        .clear_background=true, .background_index=255};
    for (unsigned pass = 0; pass < 2; pass++) {
        assert(present_frame_and_wait(d,&f) == ESP_OK);
        assert(gray_at(d->gray,WIDTH-1,HEIGHT-1) == rgb_gray(palette[255]));
        for (unsigned i = 0; i < 256; i++) {
            assert(gray_at(d->gray,7+2*i,9) == rgb_gray(palette[i]));
            assert(gray_at(d->gray,8+2*i,10) == rgb_gray(palette[i]));
            palette[i] ^= 0xffff;
        }
    }
}

static void test_read_recovery(solar_os_it8951_t *d)
{
    uint8_t info[40];
    assert(command(d,0x0302,NULL,0) == ESP_OK);
    fail_after = 5; /* Preamble, dummy and three response words succeed. */
    assert(transfer(d,0x1000,NULL,info,sizeof(info)) == ESP_FAIL);
    assert(info_words == 3 && !held && !selected);
    assert(command(d,0x0302,NULL,0) == ESP_OK);
    assert(transfer(d,0x1000,NULL,info,sizeof(info)) == ESP_OK);
    assert(info_words == 20 && !held && !selected);
    assert((((unsigned)info[0] << 8) | info[1]) == WIDTH);
    assert((((unsigned)info[2] << 8) | info[3]) == HEIGHT);
}

static void test_device_info(solar_os_it8951_t *d)
{
    assert(!strcmp(d->firmware_version,"Seeed_v.0.1") && !strcmp(d->waveform_version,"3M29T"));
    assert(strstr(solar_os_it8951_mode(d),"fw=Seeed_v.0.1"));
    assert(strstr(solar_os_it8951_mode(d),"lut-id=3M29T"));
    uint8_t raw[16]; char text[17];
    memset(raw,'X',sizeof(raw)); device_info_string(text,raw);
    assert(strlen(text)==16 && text[16]=='\0');
    memcpy(raw,"name  \0",7); device_info_string(text,raw); assert(!strcmp(text,"name"));
    raw[0]=1;raw[1]=0;device_info_string(text,raw);assert(!strcmp(text,"?"));
    raw[0]='A';raw[1]=0xff;device_info_string(text,raw);assert(!strcmp(text,"A"));
    raw[0]=0;device_info_string(text,raw);assert(!strcmp(text,""));
}

static solar_os_it8951_t *graphics_fixture;
static void replace_mono_during_upload(void)
{
    solar_os_it8951_t *d = graphics_fixture;
    memset(d->mono,0xff,MONO_BYTES);
    d->mono[WIDTH-1U] &= (uint8_t)~1U;
    assert(solar_os_it8951_present(d) == ESP_OK);
    /* Native canvas data is borrowed only for the submission call. */
    memset(d->mono,0,MONO_BYTES);
}
static void test_scaled_indexed_rows(solar_os_it8951_t *d)
{
    uint16_t palette[16];
    uint8_t pixels[5*7];
    for (unsigned i=0;i<16;i++) palette[i]=(uint16_t)(i*0x1111U);
    for (unsigned i=0;i<sizeof(pixels);i++) pixels[i]=(uint8_t)(i%16U);
    solar_os_display_raster_t f = {.data=pixels,.data_size=sizeof(pixels),
        .source_width=5,.source_height=5,.source_stride=7,.x=4,.y=5,.width=18,.height=23,
        .format=SOLAR_OS_DISPLAY_FORMAT_INDEX8,.palette_rgb565=palette,.palette_size=16,
        .clear_background=true,.background_index=15};
    const unsigned heights[]={23,3,8,16,1};
    for (unsigned pass=0;pass<5;pass++) {
        f.height=heights[pass];
        assert(present_frame_and_wait(d,&f)==ESP_OK);
        for (unsigned y=0;y<f.height;y++) for (unsigned x=0;x<f.width;x++) {
            const unsigned sx=x*5U/f.width,sy=y*5U/f.height;
            const uint8_t expected=rgb_gray(palette[pixels[sy*7+sx]]);
            const unsigned nx=f.x+x,ny=f.y+y;
            assert(gray_at(d->gray,nx,ny)==expected);
            assert(!!(d->mono[(size_t)(ny/8U)*WIDTH+nx] & (1U << (ny&7U)))==(expected>=8));
        }
        assert(gray_at(d->gray,f.x-1,f.y)==15 && gray_at(d->gray,f.x+f.width,f.y)==15);
        f.clear_background=false;
        const unsigned before=updates;
        assert(present_frame_and_wait(d,&f)==ESP_OK && updates==before);
        f.clear_background=true;
    }
}

static void replace_graphics_during_upload(void)
{
    solar_os_it8951_t *d = graphics_fixture;
    uint8_t index = 0;
    uint16_t palette = 0x8410;
    solar_os_display_raster_t patch = {.data=&index, .data_size=1,
        .source_width=1, .source_height=1, .source_stride=1, .width=1, .height=1,
        .format=SOLAR_OS_DISPLAY_FORMAT_INDEX8, .palette_rgb565=&palette, .palette_size=1};
    const unsigned before = updates;
    const uint8_t active = image_gray_at(d,d->gray,0,0);
    assert(solar_os_it8951_present_frame(d,&patch)==ESP_OK);
    palette=0x4208; patch.x=WIDTH-1; patch.y=HEIGHT-1;
    assert(solar_os_it8951_present_frame(d,&patch)==ESP_OK);
    palette=0xffff; patch.x=patch.y=0;
    assert(solar_os_it8951_present_frame(d,&patch)==ESP_OK);
    assert(updates==before && image_gray_at(d,d->gray,0,0)==active);
    /* Neither the borrowed palette nor source pixels may outlive submission. */
    palette=0; index=99;
}

static void test_graphics_queue(solar_os_it8951_t *d)
{
    assert(solar_os_it8951_set_mode(d,"refresh=auto")==ESP_OK);
    uint8_t index=0;
    uint16_t palette=0x8410;
    solar_os_display_raster_t f = {.data=&index, .data_size=1,
        .source_width=1, .source_height=1, .source_stride=1, .width=1, .height=1,
        .format=SOLAR_OS_DISPLAY_FORMAT_INDEX8, .palette_rgb565=&palette, .palette_size=1,
        .clear_background=true};
    const unsigned before=updates;
    assert(solar_os_it8951_present_frame(d,&f)==ESP_OK && updates==before);
    assert(d->graphics_scan && d->gray_pending && !d->mono_shadow);
    assert(d->scan_worker.frame_size==sizeof(uint32_t));
    graphics_fixture=d; during_upload=replace_graphics_during_upload;
    palette=0; index=99;
    pump(&d->scan_worker);
    assert(!during_upload && updates==before+1 && gray_at(d->gray,0,0)==8);
    assert(d->scan_worker.has_target && d->scan_worker.stats.coalesced==2);
    pump(&d->scan_worker);
    assert(updates==before+2 && gray_at(d->gray,0,0)==15 && gray_at(d->gray,WIDTH-1,HEIGHT-1)==4);
    assert(uploaded[WIDTH-1]==255 && uploaded[(size_t)(HEIGHT-1)*WIDTH]==4*17);
    /* An identical target must not refresh; a pending frame must be cancelled
     * safely when returning to native text or changing power/mode. */
    index=0; palette=0xffff; f.clear_background=false;
    assert(solar_os_it8951_present_frame(d,&f)==ESP_OK);
    pump(&d->scan_worker);
    assert(updates==before+2);
    palette=0;
    assert(solar_os_it8951_present_frame(d,&f)==ESP_OK);
    const uint32_t errors = d->refresh_errors, resets = d->controller_resets;
    fail_after=2; pump(&d->scan_worker);
    assert(d->ready && d->shadow_valid && gray_at(d->gray,0,0)==0);
    assert(d->refresh_errors == errors+1U && d->controller_resets == resets+1U);
    assert(d->last_error == ESP_OK);
    assert(solar_os_it8951_present_frame(d,&f)==ESP_OK);
    assert(solar_os_it8951_set_power_save(d,true)==ESP_OK);
    assert(!d->gray_pending && !d->scan_worker.task);
    assert(solar_os_it8951_present_frame(d,&f)==ESP_ERR_INVALID_STATE);
    assert(solar_os_it8951_set_power_save(d,false)==ESP_OK);
    worker_init_fail=true;
    palette=0x8410;
    assert(solar_os_it8951_present_frame(d,&f)==ESP_OK && !d->scan_worker.task && !d->gray_pending);
    assert(gray_at(d->gray,0,0)==8);
    worker_init_fail=false;
    assert(solar_os_it8951_present(d)==ESP_OK && !d->graphics_scan && d->mono_shadow);
    pump(&d->scan_worker);
    assert(d->last_bpp==1);
}

static void test_retained_baseline(solar_os_it8951_t *d)
{
    assert(solar_os_it8951_set_mode(d,"grayscale=4")==ESP_OK);
    assert(solar_os_it8951_set_mode(d,"refresh=full")==ESP_OK);
    memset(d->mono,0xff,MONO_BYTES);
    assert(solar_os_it8951_present_sync(d)==ESP_OK);
    assert(solar_os_it8951_set_mode(d,"refresh=auto")==ESP_OK);
    const unsigned before=updates, cleanups=d->maintenance.cleanups;
    assert(solar_os_it8951_present(d)==ESP_OK); pump(&d->scan_worker);
    assert(updates==before && d->scan_worker.stats.scans==0);
    /* A white raster patch over the same white retained image changes neither
     * encoding nor pixels, despite switching the presentation path. */
    const uint8_t white=0;
    const solar_os_display_raster_t patch={.data=&white, .data_size=1,
        .source_width=1, .source_height=1, .source_stride=1, .width=1, .height=1,
        .format=SOLAR_OS_DISPLAY_FORMAT_MONO1};
    assert(present_frame_and_wait(d,&patch)==ESP_OK && d->graphics_scan);
    assert(updates==before && d->maintenance.cleanups==cleanups);
    assert(solar_os_it8951_present(d)==ESP_OK && d->scan_worker.has_target);
    /* Idempotent configuration and resume must preserve an accepted target. */
    assert(solar_os_it8951_set_mode(d,"refresh=auto")==ESP_OK && d->scan_worker.has_target);
    assert(solar_os_it8951_set_mode(d,"grayscale=4")==ESP_OK && d->scan_worker.has_target);
    assert(solar_os_it8951_set_power_save(d,false)==ESP_OK && d->scan_worker.has_target);
    pump(&d->scan_worker);
    assert(updates==before && d->maintenance.cleanups==cleanups && d->scan_worker.stats.scans==0);
    d->mono[17]^=1;
    assert(solar_os_it8951_present(d)==ESP_OK); pump(&d->scan_worker);
    assert(updates==before+1 && last_waveform==1 && d->scan_worker.stats.scans==1);
}

static void test_spi_clocks(solar_os_it8951_t *d)
{
    static const char *invalid[] = {"spi-khz=", "spi-khz=-1", "spi-khz=+10000",
        "spi-khz=99", "spi-khz=20001", "spi-khz=10000junk", "spi-khz= 10000",
        "spi-khz=99999999999999999999999", "spi-read-khz=99", "spi-read-khz=20001"};
    const uint32_t resets = d->controller_resets, errors = d->refresh_errors;
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++)
        assert(solar_os_it8951_set_mode(d,invalid[i]) == ESP_ERR_INVALID_ARG);
    assert(d->config.clock_hz == 10000000U && d->spi_read_clock_hz == 10000000U);
    d->mono[18] ^= 1;
    assert(solar_os_it8951_present(d) == ESP_OK);
    const TaskHandle_t task = d->scan_worker.task;
    assert(solar_os_it8951_set_mode(d,"spi-khz=20000") == ESP_OK);
    assert(d->scan_worker.task == task && d->scan_worker.has_target && d->shadow_valid);
    assert(d->spi_clock_khz == 10000); /* The override has not transferred yet. */
    pump(&d->scan_worker);
    assert(last_write_clock_khz == 20000 && last_read_clock_khz == 10000);
    assert(d->spi_clock_khz == 20000 && d->spi_read_clock_khz == 10000);
    assert(d->controller_resets == resets && d->refresh_errors == errors);
    assert(strstr(solar_os_it8951_mode(d),"spi=20000kHz read-spi=10000kHz spi-target=20000/10000kHz"));
    assert(solar_os_it8951_set_mode(d,"spi-read-khz=4000") == ESP_OK);
    uint16_t packed;
    assert(read_reg(d,0x0004,&packed) == ESP_OK && packed == 1);
    assert(last_write_clock_khz == 20000 && last_read_clock_khz == 4000);
    assert(d->spi_read_clock_khz == 4000);
    assert(solar_os_it8951_set_mode(d,"spi-read-khz=20000") == ESP_OK);
    assert(read_reg(d,0x0004,&packed) == ESP_OK && packed == 1);
    assert(d->spi_read_clock_khz == 20000); /* Same rate as the previous write. */
    assert(solar_os_it8951_set_mode(d,"spi-khz=15000") == ESP_OK);
    assert(read_reg(d,0x0004,&packed) == ESP_OK && packed == 1);
    assert(d->spi_clock_khz == 16000 && last_write_clock_khz == 16000);
    assert(strstr(solar_os_it8951_mode(d),"spi-target=15000/20000kHz"));
    assert(solar_os_it8951_set_mode(d,"spi-khz=10000") == ESP_OK);
    assert(solar_os_it8951_set_mode(d,"spi-read-khz=10000") == ESP_OK);
    assert(read_reg(d,0x0004,&packed) == ESP_OK && packed == 1);
    assert(last_write_clock_khz == 10000 && last_read_clock_khz == 10000);
    assert(d->controller_resets == resets && d->refresh_errors == errors);
}

static void test_default_spi_clocks(void)
{
    solar_os_it8951_t d;
    solar_os_it8951_config_t cfg = config();
    cfg.clock_hz = 20000000U;
    assert(solar_os_it8951_init(&d,&cfg) == ESP_OK);
    /* Cold startup retains the established handshake timing. The first image
     * selects fast writes without increasing the read clock. */
    assert(last_write_clock_khz == 10000 && last_read_clock_khz == 10000);
    assert(d.config.clock_hz == 20000000U && d.spi_read_clock_hz == 10000000U);
    assert(d.spi_clock_khz == 10000 && d.spi_read_clock_khz == 10000);
    assert(solar_os_it8951_present(&d) == ESP_OK); pump(&d.scan_worker);
    assert(d.spi_clock_khz == 20000 && d.spi_read_clock_khz == 10000);
    assert(last_write_clock_khz == 20000 && last_read_clock_khz == 10000);
    assert(d.controller_resets == 1U && d.refresh_errors == 0U);
    assert(solar_os_it8951_set_power_save(&d,true) == ESP_OK);
    assert(solar_os_it8951_set_power_save(&d,false) == ESP_OK);
    assert(d.config.clock_hz == 20000000U && d.spi_read_clock_hz == 10000000U);
    assert(solar_os_it8951_present(&d) == ESP_OK); pump(&d.scan_worker);
    assert(d.spi_clock_khz == 20000 && d.spi_read_clock_khz == 10000);
    assert(d.controller_resets == 2U && d.refresh_errors == 0U);
    solar_os_it8951_deinit(&d);
}

static void check_binary_panel(const solar_os_it8951_t *d)
{
    assert(d->last_bpp == 1 && image_format == 0x0030);
    for (unsigned y = 0; y < HEIGHT; y++)
        for (unsigned x = 0; x < WIDTH; x++)
            assert(uploaded[(size_t)y*WIDTH+x] ==
                image_gray_at(d,d->gray,WIDTH-1U-x,y)*17U);
}

static unsigned white_square(const solar_os_it8951_t *d, unsigned x, unsigned y)
{
    unsigned white = 0;
    for (unsigned yy = y; yy < y+4U; yy++)
        for (unsigned xx = x; xx < x+4U; xx++)
            if (image_gray_at(d,d->gray,xx,yy)) ++white;
    return white;
}

static void test_binary_storage(void)
{
    assert(allocated_bytes == 0);
    solar_os_it8951_t d;
    solar_os_it8951_config_t cfg = config();
    assert(solar_os_it8951_init(&d,&cfg) == ESP_OK);
    assert(solar_os_it8951_set_mode(&d,"grayscale=16") == ESP_OK);
    const uint16_t palette[] = {0,0x8410,0xffff};
    uint8_t pixel = 1;
    solar_os_display_raster_t f = {.data=&pixel,.data_size=1,
        .source_width=1,.source_height=1,.source_stride=1,.width=16,.height=4,
        .format=SOLAR_OS_DISPLAY_FORMAT_INDEX8,.palette_rgb565=palette,.palette_size=3,
        .clear_background=true,.background_index=2};
    assert(present_frame_and_wait(&d,&f) == ESP_OK && d.last_bpp == 4);
    const size_t grayscale_bytes = allocated_bytes;
    assert(allocation_bytes(d.gray) == GRAY_BYTES && allocation_bytes(d.gray_pending) == GRAY_BYTES);
    /* Allocation failure must preserve an accepted but unscanned image. */
    pixel=0; f.clear_background=false;
    assert(solar_os_it8951_present_frame(&d,&f) == ESP_OK);
    uint8_t *pending = d.gray_pending;
    const TaskHandle_t task = d.scan_worker.task;
    fail_allocation_size = BINARY_BYTES;
    assert(solar_os_it8951_set_mode(&d,"grayscale=2") == ESP_ERR_NO_MEM);
    assert(d.gray_levels == 16 && d.gray_pending == pending && d.scan_worker.task == task);
    assert(d.scan_worker.has_target && allocated_bytes == grayscale_bytes);
    pump(&d.scan_worker);
    assert(gray_at(d.gray,0,0) == 0);
    pixel=1;
    assert(solar_os_it8951_present_frame(&d,&f) == ESP_OK);
    assert(solar_os_it8951_set_mode(&d,"grayscale=2") == ESP_OK);
    assert(!d.scan_worker.task && !d.gray_pending && !d.shadow_valid);
    assert(white_square(&d,0,0) == 9); /* Ordered binary pixels retain midgray coverage. */
    assert(present_frame_and_wait(&d,&f) == ESP_OK);
    assert(allocation_bytes(d.gray) == BINARY_BYTES && allocation_bytes(d.gray_pending) == BINARY_BYTES);
    assert(grayscale_bytes-allocated_bytes == 2U*(GRAY_BYTES-BINARY_BYTES));
    printf("1bpp graphics: %zu bytes saved\n",grayscale_bytes-allocated_bytes);
    assert(strstr(solar_os_it8951_mode(&d),"storage-bpp=1 image-bytes=328536"));
    check_binary_panel(&d);
    const unsigned before = updates;
    assert(present_frame_and_wait(&d,&f) == ESP_OK && updates == before);
    const TaskHandle_t binary_task = d.scan_worker.task;
    assert(solar_os_it8951_present_frame(&d,&f) == ESP_OK);
    assert(solar_os_it8951_set_mode(&d,"grayscale=2") == ESP_OK);
    assert(d.scan_worker.task == binary_task && d.scan_worker.has_target);
    pump(&d.scan_worker);
    /* A producer can replace patches while the compact frame uploads. */
    pixel=0; f.clear_background=true;
    assert(solar_os_it8951_present_frame(&d,&f) == ESP_OK);
    graphics_fixture=&d; during_upload=replace_graphics_during_upload;
    pump(&d.scan_worker);
    assert(!during_upload && d.scan_worker.has_target);
    pump(&d.scan_worker);
    assert(image_gray_at(&d,d.gray,0,0) == 15);
    assert(image_gray_at(&d,d.gray,WIDTH-1U,HEIGHT-1U) == 0);
    check_binary_panel(&d);
    /* Unaligned partial patches use the same fixed dither coordinates. */
    pixel=1; f.clear_background=false;
    f.x=3; f.y=5; f.width=8; f.height=8;
    assert(present_frame_and_wait(&d,&f) == ESP_OK && white_square(&d,4,8) == 9);
    check_binary_panel(&d);
    /* Both pure endpoints remain exact, including a dithered clear color. */
    for (unsigned index = 0; index < 3; index++) {
        pixel=(uint8_t)index; f.clear_background=true; f.background_index=(uint8_t)index;
        assert(present_frame_and_wait(&d,&f) == ESP_OK);
        const unsigned expected = index == 0 ? 0U : index == 2 ? 16U : 9U;
        assert(white_square(&d,100,100) == expected);
        check_binary_panel(&d);
    }
    /* Indexed, packed and RGB rasters cover all rotations without a gray
     * allocation, including rectangles not aligned to a packed-byte boundary. */
    const uint8_t indices[] = {0,1,2}, index2=0x24, mono=1;
    const uint8_t rgb[] = {0,0,0x84,0x10,0xff,0xff};
    for (unsigned rotation = 0; rotation < 4; rotation++) {
        static const u8g2_cb_t *const callbacks[] = {U8G2_R0,U8G2_R1,U8G2_R2,U8G2_R3};
        u8g2_SetDisplayRotation(&d.u8g2,callbacks[rotation]);
        f.x=13; f.y=17; f.width=11; f.height=9;
        f.source_width=3; f.source_height=1; f.background_index=2;
        for (unsigned format = 0; format < 4; format++) {
            f.format=(solar_os_display_format_t)format;
            f.data=format==SOLAR_OS_DISPLAY_FORMAT_INDEX8 ? indices :
                format==SOLAR_OS_DISPLAY_FORMAT_INDEX2 ? &index2 :
                format==SOLAR_OS_DISPLAY_FORMAT_RGB565 ? rgb : &mono;
            f.data_size=f.source_stride=format==SOLAR_OS_DISPLAY_FORMAT_INDEX8 ? 3U :
                format==SOLAR_OS_DISPLAY_FORMAT_RGB565 ? 6U : 1U;
            assert(present_frame_and_wait(&d,&f) == ESP_OK);
            check_binary_panel(&d);
            assert(allocation_bytes(d.gray) == BINARY_BYTES && allocation_bytes(d.gray_pending) == BINARY_BYTES);
        }
    }
    u8g2_SetDisplayRotation(&d.u8g2,U8G2_R0);
    assert(solar_os_it8951_set_mode(&d,"refresh=full") == ESP_OK);
    memset(d.mono,0xff,MONO_BYTES);
    assert(solar_os_it8951_present_sync(&d) == ESP_OK);
    assert(solar_os_it8951_set_mode(&d,"refresh=auto") == ESP_OK);
    test_sparse_uploads(&d);
    /* Compact buffers also serve native U8g2 text and the final padded tile. */
    assert(solar_os_it8951_set_mode(&d,"refresh=full") == ESP_OK);
    memset(d.mono,0xff,MONO_BYTES);
    d.mono[0]=0xfe;
    const size_t last = (size_t)((HEIGHT-1U)/8U)*WIDTH+WIDTH-1U;
    d.mono[last]&=(uint8_t)~8U;
    assert(solar_os_it8951_present_sync(&d) == ESP_OK);
    assert(upload_bytes == BINARY_BYTES && image_gray_at(&d,d.gray,0,0) == 0);
    assert(image_gray_at(&d,d.gray,WIDTH-1U,HEIGHT-1U) == 0);
    check_binary_panel(&d);
    assert(solar_os_it8951_set_mode(&d,"refresh=auto") == ESP_OK);
    assert(solar_os_it8951_present(&d) == ESP_OK); pump(&d.scan_worker);
    const unsigned padded_before = updates;
    d.mono[last]^=0xf0;
    assert(solar_os_it8951_present(&d) == ESP_OK); pump(&d.scan_worker);
    assert(updates == padded_before);
    d.mono[last]^=8;
    assert(solar_os_it8951_present(&d) == ESP_OK); pump(&d.scan_worker);
    assert(d.last_y == HEIGHT-1U && d.last_height == 1 && upload_bytes == 4);
    check_binary_panel(&d);
    /* Native monochrome uses the same compact queue without full-size mono
     * snapshots. A later canvas can replace its target during an upload. */
    memset(d.mono,0xff,MONO_BYTES);d.mono[100]&=(uint8_t)~1U;
    assert(solar_os_it8951_present(&d) == ESP_OK);
    assert(d.graphics_scan && d.gray_pending && !d.mono_shadow);
    assert(d.scan_worker.frame_size == sizeof(d.frame_generation));
    graphics_fixture=&d;during_upload=replace_mono_during_upload;
    pump(&d.scan_worker);assert(!during_upload && d.scan_worker.has_target);
    pump(&d.scan_worker);
    assert(image_gray_at(&d,d.gray,0,0) == 15 && image_gray_at(&d,d.gray,WIDTH-1U,0) == 0);
    check_binary_panel(&d);
    /* Returning to grayscale can fail without losing the binary queue. */
    f=(solar_os_display_raster_t){.data=&pixel,.data_size=1,
        .source_width=1,.source_height=1,.source_stride=1,.width=16,.height=4,
        .format=SOLAR_OS_DISPLAY_FORMAT_INDEX8,.palette_rgb565=palette,.palette_size=3,
        .clear_background=true,.background_index=2};
    pixel=1;
    assert(present_frame_and_wait(&d,&f) == ESP_OK);
    const size_t binary_bytes = allocated_bytes;
    pixel=0; f.clear_background=false;
    assert(solar_os_it8951_present_frame(&d,&f) == ESP_OK);
    fail_allocation_size = GRAY_BYTES;
    assert(solar_os_it8951_set_mode(&d,"grayscale=16") == ESP_ERR_NO_MEM);
    assert(d.gray_levels == 2 && d.scan_worker.has_target && allocated_bytes == binary_bytes);
    const uint32_t errors = d.refresh_errors, resets = d.controller_resets;
    fail_bulk_after=0; pump(&d.scan_worker);
    assert(d.refresh_errors == errors+1U && d.controller_resets == resets+1U);
    check_binary_panel(&d);
    assert(solar_os_it8951_set_power_save(&d,true) == ESP_OK);
    assert(solar_os_it8951_set_power_save(&d,false) == ESP_OK);
    assert(d.gray_levels == 2 && allocation_bytes(d.gray) == BINARY_BYTES);
    assert(present_frame_and_wait(&d,&f) == ESP_OK);
    check_binary_panel(&d);
    worker_init_fail=true;
    pixel=1; f.clear_background=true;
    scan_stop(&d);
    assert(present_frame_and_wait(&d,&f) == ESP_OK && !d.gray_pending && !d.scan_worker.task);
    check_binary_panel(&d);
    worker_init_fail=false;
    assert(present_frame_and_wait(&d,&f) == ESP_OK && d.gray_pending);
    assert(solar_os_it8951_set_mode(&d,"grayscale=16") == ESP_OK);
    pixel=1; f.clear_background=true;
    assert(present_frame_and_wait(&d,&f) == ESP_OK && d.last_bpp == 4);
    assert(gray_at(d.gray,0,0) == 8 && allocated_bytes == grayscale_bytes);
    for (unsigned pass = 0; pass < 3; pass++) {
        assert(solar_os_it8951_set_mode(&d,"grayscale=2") == ESP_OK);
        assert(present_frame_and_wait(&d,&f) == ESP_OK);
        assert(solar_os_it8951_set_mode(&d,pass & 1U ? "grayscale=16" : "grayscale=4") == ESP_OK);
        assert(present_frame_and_wait(&d,&f) == ESP_OK && allocated_bytes == grayscale_bytes);
    }
    assert(solar_os_it8951_present(&d) == ESP_OK); pump(&d.scan_worker);
    const size_t text_grayscale_bytes = allocated_bytes;
    assert(solar_os_it8951_set_mode(&d,"grayscale=2") == ESP_OK);
    assert(solar_os_it8951_present(&d) == ESP_OK); pump(&d.scan_worker);
    assert(text_grayscale_bytes-allocated_bytes == GRAY_BYTES-BINARY_BYTES +
        3U*MONO_BYTES-BINARY_BYTES-HEIGHT*sizeof(*d.pending_rows)-2U*sizeof(d.frame_generation));
    printf("1bpp text: %zu bytes saved\n",text_grayscale_bytes-allocated_bytes);
    solar_os_it8951_deinit(&d);
    assert(allocated_bytes == 0);
}

static uint32_t test_random(uint32_t *state)
{
    *state ^= *state << 13; *state ^= *state >> 17; *state ^= *state << 5;
    return *state;
}

static void test_compact_conversion(void)
{
    for (unsigned x=0; x<8; x++) for (unsigned y=0; y<8; y++) {
        uint8_t columns[8]={0};columns[x]=(uint8_t)(1U << y);
        assert(mono_tile_rows(columns) == (UINT64_C(0x80) >> x) << (8U*y));
    }
    uint32_t random=0x23456789;
    uint8_t *source=malloc(MONO_BYTES),*image=malloc(BINARY_BYTES),*expected=malloc(BINARY_BYTES);
    assert(source && image && expected);
    for (size_t i=0; i<MONO_BYTES; i++) source[i]=(uint8_t)test_random(&random);
    const dirty_area_t areas[]={{0,WIDTH,0,HEIGHT,NULL},{3,21,5,19,NULL},
        {WIDTH-1U,WIDTH,HEIGHT-1U,HEIGHT,NULL},{24,67,0,1,NULL}};
    uint64_t rows[HEIGHT],expected_rows[HEIGHT];
    solar_os_it8951_t d={.gray_levels=2};
    for (unsigned a=0; a<sizeof(areas)/sizeof(areas[0]); a++) {
        memset(image,0xa5,BINARY_BYTES);memcpy(expected,image,BINARY_BYTES);
        memset(rows,0,sizeof(rows));memset(expected_rows,0,sizeof(expected_rows));
        const dirty_area_t *area=&areas[a];
        for (unsigned y=area->y0; y<area->y1; y++)
            for (unsigned x=area->x0 & ~7U; x<((area->x1+7U)&~7U); x+=8U) {
                uint8_t value=0;
                for (unsigned bit=0; bit<8; bit++)
                    if (source[(size_t)(y/8U)*WIDTH+x+bit] & (1U << (y & 7U))) value|=0x80U >> bit;
                const unsigned byte=x/8U;
                const size_t offset=(size_t)y*(WIDTH/8U)+WIDTH/8U-2U-(byte & ~1U)+(byte & 1U);
                if (expected[offset]!=value) expected_rows[y]|=UINT64_C(1) << ((WIDTH-1U-x)/32U);
                expected[offset]=value;
            }
        mono_to_gray_area(&d,source,area,image,rows);
        assert(!memcmp(image,expected,BINARY_BYTES) && !memcmp(rows,expected_rows,sizeof(rows)));
    }
    free(source);free(image);free(expected);
}

static void test_fused_canvas(void)
{
    solar_os_it8951_t d;
    solar_os_it8951_config_t cfg=config();
    assert(solar_os_it8951_init(&d,&cfg)==ESP_OK);
    assert(solar_os_it8951_set_mode(&d,"grayscale=2")==ESP_OK);
    uint32_t random=0x4218951;
    for (unsigned factor=2; factor<=4; factor++) {
        const unsigned w=WIDTH/factor,h=HEIGHT/factor,stride=(w+7U)&~7U;
        const size_t bytes=(size_t)stride*((h+7U)/8U);
        uint8_t *source=malloc(bytes);assert(source);
        for (size_t i=0; i<bytes; i++) source[i]=(uint8_t)test_random(&random);
        solar_os_display_mono_canvas_t canvas={.data=source,.data_size=bytes,
            .width=w,.height=h,.stride=stride,.scale=factor,.native_buffer=d.mono,
            .native_width=WIDTH,.native_height=HEIGHT,.native_stride=WIDTH,.area={0,0,WIDTH,HEIGHT}};
        external_dma=true;
        const unsigned before=direct_calls;
        d.full_refresh=true;
        assert(solar_os_it8951_present_canvas(&d,&canvas)==ESP_OK);
        pump(&d.scan_worker);
        assert(direct_calls>before && d.last_direct_bytes>BINARY_BYTES-256U);
        d.full_refresh=false;
        check_binary_panel(&d);
        for (unsigned y=0; y<HEIGHT; y++) for (unsigned x=0; x<WIDTH; x++) {
            const unsigned sx=x/factor,sy=y/factor;
            const bool expected=sx<w && sy<h &&
                (source[(size_t)(sy/8U)*stride+sx] & (1U<<(sy&7U)));
            assert((image_gray_at(&d,d.gray,x,y)!=0)==expected);
            assert(!!(d.mono[(size_t)(y/8U)*WIDTH+x] & (1U<<(y&7U)))==expected);
        }
        const uint8_t far=image_gray_at(&d,d.gray,(w-1U)*factor,(h-1U)*factor);
        source[2]^=1U<<5;
        source[(size_t)((h-1U)/8U)*stride+w-1U]^=1U<<((h-1U)&7U);
        canvas.area=(solar_os_display_rect_t){2U*factor,5U*factor,factor,factor};
        assert(solar_os_it8951_present_canvas(&d,&canvas)==ESP_OK);
        pump(&d.scan_worker);
        assert(d.last_width<=64 && d.last_height<=16);
        assert(image_gray_at(&d,d.gray,(w-1U)*factor,(h-1U)*factor)==far);
        canvas.data_size=0;
        assert(solar_os_it8951_present_canvas(&d,&canvas)==ESP_ERR_INVALID_ARG);
        external_dma=false;free(source);
    }
    const unsigned px=40,py=64;
    d.mono[(size_t)(py/8U)*WIDTH+px]^=1U<<(py&7U);
    const bool expected=d.mono[(size_t)(py/8U)*WIDTH+px] & (1U<<(py&7U));
    u8g2_UpdateDisplayArea(&d.u8g2,px/8U,py/8U,1,1);
    u8x8_RefreshDisplay(&d.u8g2.u8x8);pump(&d.scan_worker);
    assert((image_gray_at(&d,d.gray,px,py)!=0)==expected);
    assert(d.last_width<=32 && d.last_height<=8);
    solar_os_it8951_deinit(&d);
}

static void test_ready_waits(void)
{
    solar_os_it8951_t d={.config={.busy=13}};
    const unsigned before=delay_calls;
    busy_until=now+5;
    assert(wait_ready(&d)==ESP_OK && now==busy_until && delay_calls==before);
    busy_until=now+100;
    assert(wait_ready(&d)==ESP_OK && delay_calls==before+1U);
    busy_until=0;
}

static void test_queued_block_runs(void)
{
    uint32_t random=0x13456789;
    for (unsigned levels=2; levels<=16; levels+=14) {
        const unsigned ppb=levels==2 ? 8U : 2U;
        const size_t size=image_bytes(levels);
        solar_os_it8951_t d={.gray_levels=levels};
        d.frame_mutex=xSemaphoreCreateMutexStatic(&d.frame_mutex_storage);
        d.gray=malloc(size);d.gray_pending=malloc(size);
        d.pending_rows=calloc(HEIGHT,sizeof(*d.pending_rows));
        d.dirty_rows=calloc(HEIGHT,sizeof(*d.dirty_rows));
        uint8_t *expected=malloc(size);uint64_t expected_rows[HEIGHT];
        assert(d.gray && d.gray_pending && d.pending_rows && d.dirty_rows && expected);
        for (size_t i=0; i<size; i++) d.gray[i]=(uint8_t)test_random(&random);
        const uint64_t all=(UINT64_C(1) << ((WIDTH+31U)/32U))-1U;
        for (unsigned pattern=0; pattern<4; pattern++) {
            memcpy(d.gray_pending,d.gray,size);memcpy(expected,d.gray,size);
            memset(expected_rows,0,sizeof(expected_rows));
            d.pending_x0=0;d.pending_x1=WIDTH;d.pending_y0=37;d.pending_y1=49;
            dirty_area_t bounds={WIDTH,0,HEIGHT,0,NULL};
            for (unsigned y=37; y<49; y++) {
                d.pending_rows[y]=pattern==2 ?
                    (((uint64_t)test_random(&random)<<32)|test_random(&random)) & all : all;
                uint8_t *row=d.gray_pending+(size_t)y*(WIDTH/ppb);
                if (pattern==1 || pattern==2)
                    for (unsigned i=0; i<WIDTH/ppb; i++) row[i]^=(uint8_t)test_random(&random);
                if (pattern==3) {row[0]^=0xff;row[WIDTH/ppb-1U]^=0xff;}
                for (unsigned column=0; column<(WIDTH+31U)/32U; column++) {
                    if (!(d.pending_rows[y] & (UINT64_C(1)<<column))) continue;
                    const unsigned right=(column+1U)*32U < WIDTH ? (column+1U)*32U : WIDTH;
                    const unsigned x=WIDTH-right,width=right-column*32U;
                    const size_t offset=(size_t)y*(WIDTH/ppb)+(levels==2 ? column*4U : x/ppb);
                    if (!memcmp(expected+offset,d.gray_pending+offset,width/ppb)) continue;
                    expected_rows[y]|=UINT64_C(1)<<column;
                    memcpy(expected+offset,d.gray_pending+offset,width/ppb);
                    if (x<bounds.x0) bounds.x0=x;
                    if (x+width>bounds.x1) bounds.x1=x+width;
                    if (y<bounds.y0) bounds.y0=y;
                    bounds.y1=y+1U;
                }
            }
            const dirty_area_t actual=take_graphics_target(&d);
            assert(!memcmp(d.gray,expected,size) && !memcmp(d.dirty_rows,expected_rows,sizeof(expected_rows)));
            assert(actual.x0==bounds.x0 && actual.x1==bounds.x1 && actual.y0==bounds.y0 && actual.y1==bounds.y1);
            for (unsigned y=0; y<HEIGHT; y++) assert(!d.pending_rows[y]);
            assert(!d.pending_x1 && !d.pending_y1);
        }
        free(d.gray);free(d.gray_pending);free(d.pending_rows);free(d.dirty_rows);free(expected);
    }
}

int main(void)
{
    test_compact_conversion();
    test_queued_block_runs();
    solar_os_it8951_t d;
    solar_os_it8951_config_t cfg = config();
    cfg.vcom_mv = 2330; assert(solar_os_it8951_init(&d, &cfg) == ESP_ERR_INVALID_ARG);
    cfg = config(); cfg.ite_power = cfg.cs;
    assert(solar_os_it8951_init(&d, &cfg) == ESP_ERR_INVALID_ARG);
    cfg = config(); cfg.clock_hz = 40000000;
    assert(solar_os_it8951_init(&d,&cfg) == ESP_ERR_INVALID_ARG);
    cfg = config(); overspeed = true;
    assert(solar_os_it8951_init(&d,&cfg) == ESP_ERR_NOT_SUPPORTED && !d.spi);
    overspeed = false;
    cfg = config(); wrong_panel = true;
    assert(solar_os_it8951_init(&d, &cfg) == ESP_ERR_INVALID_RESPONSE);
    assert(!d.spi && !levels[11] && !levels[21] && !held && !selected);
    wrong_panel = false;
    fail_power_pin = 21;
    assert(solar_os_it8951_init(&d, &cfg) == ESP_FAIL);
    assert(!d.spi && !levels[11] && !levels[21]);
    sleep_power_losses = 0;
    assert(solar_os_it8951_init(&d, &cfg) == ESP_OK);
    assert(!sleep_power_losses);
    assert(sleep_retained[10] && sleep_retained[12] && sleep_retained[13]);
    assert(d.ready && d.image_address == 0x123456 && !updates);
    assert(d.controller_resets == 1U && d.refresh_errors == 0U);
    assert(d.gray_levels == 4 && strstr(solar_os_it8951_mode(&d),"grayscale=4"));
    assert(!strcmp(solar_os_it8951_mode(NULL),"refresh=auto grayscale=4"));
    assert(info_words == 20 && !info_word_pending);
    assert(d.spi_clock_khz == 10000 && strstr(solar_os_it8951_mode(&d),"spi=10000kHz"));
    assert(d.scan_worker.task && d.mono_shadow);
    assert(d.temperature_c == 27 && temperature_writes == 1 && commanded_temperature == 27);
    temperature_sample = 31; now = d.temperature_due;
    assert(update_temperature(&d) == ESP_OK && d.temperature_c == 31 && temperature_writes == 2);
    temperature_error = ESP_FAIL; now = d.temperature_due;
    assert(update_temperature(&d) == ESP_OK && d.temperature_c == 31 && temperature_writes == 2);
    temperature_error = ESP_OK; temperature_sample = 99; now = d.temperature_due;
    assert(update_temperature(&d) == ESP_OK && d.temperature_c == 31 && temperature_writes == 2);
    temperature_sample = 25; now = d.temperature_due;
    assert(update_temperature(&d) == ESP_OK && commanded_temperature == 25 && temperature_writes == 3);
    assert(u8g2_GetDisplayWidth(&d.u8g2) == WIDTH && u8g2_GetDisplayHeight(&d.u8g2) == HEIGHT);
    /* Exercise real U8g2 addressing above 64 KiB, the final row and padded tiles. */
    u8g2_SetDrawColor(&d.u8g2, 0);
    u8g2_DrawPixel(&d.u8g2, 0, 0);
    u8g2_DrawPixel(&d.u8g2, WIDTH - 1, HEIGHT - 1);
    assert(d.mono[0] == 0xfe && d.mono[(size_t)((HEIGHT-1)/8)*WIDTH + WIDTH-1] == 0xf7);
    assert(solar_os_it8951_present_sync(&d) == ESP_OK && updates == 1 && last_waveform == 2);
    assert(!d.scan_worker.task && !d.mono_shadow);
    assert(upload_bytes == (size_t)WIDTH*HEIGHT/8U);
    assert(uploaded[WIDTH-1] == 0 && uploaded[0] == 255);
    assert(uploaded[(size_t)(HEIGHT-1)*WIDTH] == 0);
    assert(solar_os_it8951_present_sync(&d) == ESP_OK && updates == 1);
    d.mono[1] = 1;
    assert(solar_os_it8951_present_sync(&d) == ESP_OK && last_waveform == 1);
    assert(d.last_x == 1856 && d.last_y == 1 && d.last_width == 16 && d.last_height == 7);
    assert(upload_bytes == 14 && uploaded[(size_t)7*WIDTH+WIDTH-2] == 0);
    assert(uploaded[WIDTH-2] == 255); /* unchanged neighboring row */
    assert(strstr(solar_os_it8951_mode(&d), "bpp=1"));
    const unsigned before_noop = updates;
    assert(solar_os_it8951_present_sync(&d) == ESP_OK && updates == before_noop);
    /* Two disjoint edits must include the unchanged gap and both edges. */
    d.mono[0] ^= 2U;
    d.mono[(size_t)((HEIGHT-1)/8)*WIDTH+WIDTH-1] ^= 8U;
    assert(solar_os_it8951_present_sync(&d) == ESP_OK);
    assert(d.last_x == 0 && d.last_width == WIDTH && d.last_y == 1 && d.last_height == HEIGHT-1);
    assert(uploaded[WIDTH+WIDTH-1] == 0 && uploaded[(size_t)(HEIGHT-1)*WIDTH] == 255);
    assert(registers[0x208] == 0x3456 && registers[0x20a] == 0x12);
    for (unsigned i = 0; i < 18; i++) {
        d.mono[4] ^= 1;
        assert(solar_os_it8951_present_sync(&d) == ESP_OK);
    }
    assert(last_waveform == 2); /* clean after twenty binary updates */
    assert(d.last_width == WIDTH && d.last_height == HEIGHT);
    assert(solar_os_it8951_set_mode(&d,"cleanup-interval=3600")==ESP_OK);
    for (unsigned i=0; i<25; ++i) {
        d.mono[16]^=1;
        assert(solar_os_it8951_present_sync(&d)==ESP_OK && last_waveform==1);
    }
    now+=3600000000LL;
    const unsigned idle_updates=updates;
    assert(solar_os_it8951_present_sync(&d)==ESP_OK && updates==idle_updates);
    d.mono[16]^=1;
    assert(solar_os_it8951_present_sync(&d)==ESP_OK && last_waveform==2);
    assert(d.last_width==WIDTH && d.last_height==HEIGHT && d.maintenance.last_reason==SOLAR_OS_EPD_REASON_TIME);
    assert(solar_os_it8951_set_mode(&d,"cleanup-interval=0")==ESP_OK);
    assert(solar_os_it8951_set_mode(&d,"refresh=full") == ESP_OK);
    assert(solar_os_it8951_present_sync(&d) == ESP_OK && last_waveform == 2);
    assert(solar_os_it8951_set_mode(&d,"refresh=auto") == ESP_OK);
    /* Full-fidelity fixtures explicitly select the optional 16-level mode. */
    assert(solar_os_it8951_set_mode(&d,"grayscale=16") == ESP_OK);
    const uint16_t palette[] = {0, 0x8410, 0xffff};
    uint8_t pixels[] = {0, 1, 2};
    solar_os_display_raster_t f = {.data=pixels, .data_size=3, .source_width=3, .source_height=1,
        .source_stride=3, .x=5, .y=7, .width=3, .height=1, .format=SOLAR_OS_DISPLAY_FORMAT_INDEX8,
        .palette_rgb565=palette, .palette_size=3};
    assert(present_frame_and_wait(&d, &f) == ESP_OK && last_waveform == 2);
    assert(image_format == 0x0120 && upload_bytes == (size_t)WIDTH*HEIGHT/2U);
    assert(!(registers[0x113a] & 4U));
    assert(gray_at(d.gray,5,7) == 0 && gray_at(d.gray,6,7) == 8 && gray_at(d.gray,7,7) == 15);
    assert(uploaded[(size_t)7*WIDTH+WIDTH-1-6] == 136);
    pixels[1] = 0;
    assert(present_frame_and_wait(&d,&f) == ESP_OK);
    /* All gray pixels were removed: bitmap mode needs a full new encoding. */
    assert(image_format == 0x0030 && upload_bytes == (size_t)WIDTH*HEIGHT/8U);
    pixels[1] = 1;
    assert(present_frame_and_wait(&d,&f) == ESP_OK);
    const uint8_t gray_pixel = 2;
    const uint16_t shades[] = {0,0x8410,0x4208};
    solar_os_display_raster_t dot = f;
    dot.data = &gray_pixel; dot.data_size = 1;
    dot.source_width = dot.width = dot.source_stride = 1;
    dot.x = 6; dot.palette_rgb565 = shades;
    assert(present_frame_and_wait(&d,&dot) == ESP_OK);
    assert(image_format == 0x0120 && d.last_width == 16 && d.last_height == 1);
    assert(upload_bytes == 8 && uploaded[(size_t)7*WIDTH+WIDTH-1-6] == 4*17);
    assert(present_frame_and_wait(&d,&f) == ESP_OK);
    const uint8_t packed_pixels = 0x24; /* LSB-first palette indices 0,1,2 */
    solar_os_display_raster_t packed = f;
    packed.data = &packed_pixels; packed.data_size = 1; packed.source_stride = 1;
    packed.format = SOLAR_OS_DISPLAY_FORMAT_INDEX2;
    assert(present_frame_and_wait(&d,&packed) == ESP_OK);
    assert(gray_at(d.gray,5,7) == 0 && gray_at(d.gray,6,7) == 8 && gray_at(d.gray,7,7) == 15);
    const uint8_t rgb_pixels[] = {0xf8,0x00,0x00,0x1f}; /* wire-order red and blue */
    packed.data = rgb_pixels; packed.data_size = 4; packed.source_stride = 4;
    packed.format = SOLAR_OS_DISPLAY_FORMAT_RGB565; packed.source_width = packed.width = 2;
    assert(present_frame_and_wait(&d,&packed) == ESP_OK);
    assert(gray_at(d.gray,5,7) == 5 && gray_at(d.gray,6,7) == 2);
    const unsigned before = updates;
    pixels[1] = 3;
    assert(present_frame_and_wait(&d, &f) == ESP_ERR_INVALID_ARG && updates == before);
    pixels[1] = 1; f.data_size=2;
    assert(present_frame_and_wait(&d, &f) == ESP_ERR_INVALID_SIZE);
    f.data_size=3;
    assert(solar_os_it8951_present_sync(&d) == ESP_OK && last_waveform == 2);
    /* DU must not be used to transition old gray pixels back to monochrome. */
    for (unsigned r = 0; r < 4; r++) {
        static const u8g2_cb_t *const cb[] = {U8G2_R0,U8G2_R1,U8G2_R2,U8G2_R3};
        u8g2_SetDisplayRotation(&d.u8g2, cb[r]);
        f.x=0; f.y=0; f.width=3; f.height=1;
        assert(present_frame_and_wait(&d, &f) == ESP_OK);
        static const unsigned xs[] = {1, WIDTH-1, WIDTH-2, 0};
        static const unsigned ys[] = {0, 1, HEIGHT-1, HEIGHT-2};
        assert(gray_at(d.gray,xs[r],ys[r]) == 8);
    }
    u8g2_SetDisplayRotation(&d.u8g2, U8G2_R0);
    const uint8_t white_pixel = 1;
    f = (solar_os_display_raster_t){.data=&white_pixel, .data_size=1,
        .source_width=1, .source_height=1, .source_stride=1, .width=1, .height=1,
        .format=SOLAR_OS_DISPLAY_FORMAT_MONO1, .palette_inverted=true,
        .clear_background=true, .background_index=0};
    assert(present_frame_and_wait(&d,&f) == ESP_OK);
    assert(gray_at(d.gray,0,0) == 15 && gray_at(d.gray,1,0) == 0);
    assert(solar_os_it8951_set_power_save(&d,true) == ESP_OK);
    assert(!d.ready && !levels[11] && !levels[21]);
    assert(solar_os_it8951_set_power_save(&d,false) == ESP_OK && d.ready && !d.shadow_valid);
    fail_after = 2;
    assert(solar_os_it8951_present_sync(&d) == ESP_FAIL && !d.shadow_valid && !d.ready);
    assert(!held && !selected);
    assert(solar_os_it8951_present_sync(&d) == ESP_OK && last_waveform == 2);
    stuck_ready = true;
    d.mono[2] ^= 1;
    assert(solar_os_it8951_present_sync(&d) == ESP_ERR_TIMEOUT && !held && !selected);
    stuck_ready = false;
    assert(solar_os_it8951_present_sync(&d) == ESP_OK);
    stuck_lut = true; d.mono[3] ^= 1;
    assert(solar_os_it8951_present_sync(&d) == ESP_ERR_TIMEOUT && !d.shadow_valid);
    stuck_lut = false;
    assert(solar_os_it8951_present_sync(&d) == ESP_OK);
    /* Submitting while a refresh is pending must copy and replace the target,
     * with no SPI work and no dependency on the caller's subsequent drawing. */
    worker_init_fail = true;
    d.mono[12] ^= 4U;
    assert(solar_os_it8951_present(&d) == ESP_OK && !d.scan_worker.task);
    worker_init_fail = false;
    const unsigned before_async = updates;
    d.mono[10] ^= 1U;
    assert(solar_os_it8951_present(&d) == ESP_OK && updates == before_async);
    d.mono[11] ^= 2U;
    assert(solar_os_it8951_present(&d) == ESP_OK && updates == before_async);
    assert(d.scan_worker.stats.submitted == 2 && d.scan_worker.stats.coalesced == 1);
    uint8_t saved = d.mono[11]; d.mono[11] ^= 2U;
    pump(&d.scan_worker);
    assert(updates == before_async+1 && gray_at(d.gray,11,1) == (saved & 2U ? 15 : 0));
    assert(d.mono_shadow_valid);
    memcpy(d.mono,d.mono_shadow,MONO_BYTES);
    const size_t last_byte = (size_t)((HEIGHT-1)/8U)*WIDTH+WIDTH-1U;
    const unsigned before_padding = updates;
    d.mono[last_byte] ^= 0xf0U; /* Only the final tile's unused rows. */
    assert(solar_os_it8951_present(&d) == ESP_OK); pump(&d.scan_worker);
    assert(updates == before_padding);
    d.mono[last_byte] ^= 8U;
    assert(solar_os_it8951_present(&d) == ESP_OK); pump(&d.scan_worker);
    assert(d.last_x == 0 && d.last_y == 1403 && d.last_height == 1 && d.last_width == 32);
    assert(upload_bytes == 4); /* Only the changed pixel row, not all eight tile rows. */
    assert(uploaded[(size_t)(HEIGHT-1)*WIDTH] == (d.mono[last_byte] & 8U ? 255 : 0));
    /* Switching presentation formats releases the previous queue buffers. */
    assert(d.mono_shadow);
    assert(present_frame_and_wait(&d,&dot) == ESP_OK);
    assert(d.scan_worker.task && d.graphics_scan && d.gray_pending && !d.mono_shadow && d.last_bpp == 4);
    assert(solar_os_it8951_present(&d) == ESP_OK);
    assert(d.scan_worker.task && d.mono_shadow);
    pump(&d.scan_worker);
    assert(last_waveform == 2 && d.last_bpp == 1);
    /* A transient asynchronous error recovers even without another submission. */
    d.mono[13] ^= 1U;
    assert(solar_os_it8951_present(&d) == ESP_OK);
    fail_after = 2; pump(&d.scan_worker);
    assert(d.ready && d.shadow_valid);
    assert(solar_os_it8951_present(&d) == ESP_OK && d.scan_worker.has_target);
    assert(solar_os_it8951_set_power_save(&d,true) == ESP_OK);
    assert(!d.scan_worker.task && !d.scan_submit_ready && !d.scan_worker.has_target);
    assert(solar_os_it8951_present(&d) == ESP_ERR_INVALID_STATE);
    assert(solar_os_it8951_set_power_save(&d,false) == ESP_OK);
    assert(solar_os_it8951_present(&d) == ESP_OK);
    assert(solar_os_it8951_set_mode(&d,"refresh=full") == ESP_OK && !d.scan_worker.task);
    assert(solar_os_it8951_present_sync(&d) == ESP_OK && last_waveform == 2);
    assert(solar_os_it8951_present(&d) == ESP_OK); pump(&d.scan_worker);
    bool pending,scanned;
    const unsigned before_full_duplicate = updates;
    assert(scan_process(&d,d.scan_worker.snapshot,false,&pending,&scanned) == ESP_OK);
    assert(scanned && !pending && updates == before_full_duplicate+1 && last_waveform == 2);
    assert(solar_os_it8951_set_mode(&d,"refresh=auto")==ESP_OK);
    test_device_info(&d);
    test_scaled_indexed_rows(&d);
    test_graphics_queue(&d);
    test_indexed_palette(&d);
    test_sparse_uploads(&d);
    test_encoding_cache(&d);
    test_four_level_uploads(&d);
    test_upload_packing(&d);
    test_read_recovery(&d);
    test_retained_baseline(&d);
    test_spi_clocks(&d);
    solar_os_it8951_deinit(&d);
    assert(!held && !selected && !d.spi && removed == 4);
    cfg = config(); cfg.clock_hz = 5000000;
    assert(solar_os_it8951_init(&d,&cfg) == ESP_OK);
    assert(d.spi_clock_khz == 5000 && strstr(solar_os_it8951_mode(&d),"spi=5000kHz"));
    assert(d.spi_read_clock_hz == 5000000U);
    solar_os_it8951_deinit(&d);
    /* Expansion buses with the previous row-sized transfer limit still work. */
    spi_transfer_limit=WIDTH;
    cfg=config();
    assert(solar_os_it8951_init(&d,&cfg)==ESP_OK && d.transfer_size==WIDTH);
    assert(solar_os_it8951_set_mode(&d,"grayscale=16")==ESP_OK);
    memset(d.gray,0x88,GRAY_BYTES);
    const dirty_area_t full={0,WIDTH,0,HEIGHT,NULL};
    assert(finish_refresh(&d,false,&full)==ESP_OK && d.last_bpp==4);
    assert(upload_calls==HEIGHT/2U && upload_bytes==GRAY_BYTES);
    solar_os_it8951_deinit(&d);
    spi_transfer_limit=4096;
    test_default_spi_clocks();
    test_binary_storage();
    test_fused_canvas();
    test_ready_waits();
    assert(allocated_bytes == 0);
    assert(!sleep_power_losses);
    puts("IT8951 protocol, framebuffer, grayscale, rotation, sleep retention and recovery tests passed");
    return 0;
}

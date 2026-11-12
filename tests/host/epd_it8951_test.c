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
#include "ssd2677_memory_stubs.h"

static int64_t now;
static bool held, selected, stuck_ready, stuck_lut, wrong_panel;
static unsigned phase, preamble, current_command, arg_count, last_reg;
static unsigned updates, last_waveform, rows, removed;
static unsigned image_x, image_y, image_width, image_height, image_format;
static size_t upload_bytes;
static int fail_after = -1;
static int fail_power_pin = -1;
static uint16_t registers[0x1300];
static uint16_t args[8];
static uint8_t uploaded[(size_t)WIDTH * HEIGHT];
static unsigned levels[49];
static bool worker_init_fail;
static int16_t temperature_sample = 27;
static esp_err_t temperature_error;
static unsigned temperature_writes, commanded_temperature;
static int spi_clock_khz;
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
    w->latest = malloc(n); w->snapshot = malloc(n);
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
    free(w->latest); free(w->snapshot);
    w->latest = w->snapshot = NULL; w->task = NULL; w->has_target = false;
}
static void pump(epd_scan_worker_t *w)
{
    assert(w->task && w->enabled && w->has_target);
    memcpy(w->snapshot,w->latest,w->frame_size); w->has_target = false;
    bool pending,scanned;
    assert(w->process(w->context,w->snapshot,true,&pending,&scanned) == ESP_OK);
    assert(!pending && scanned);
}

static esp_err_t present_frame_and_wait(solar_os_it8951_t *d,
    const solar_os_display_raster_t *f)
{
    const esp_err_t err = solar_os_it8951_present_frame(d,f);
    if (err == ESP_OK && d->scan_worker.task && d->scan_worker.has_target) pump(&d->scan_worker);
    return err;
}

int64_t esp_timer_get_time(void) { return now; }
void vTaskDelay(TickType_t ticks) { assert(ticks); now += (int64_t)ticks * 1000; }
esp_err_t gpio_config(const gpio_config_t *cfg) { assert(cfg); return ESP_OK; }
int gpio_get_level(gpio_num_t pin)
{
    assert(pin == 13);
    if (stuck_ready) return 0;
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
        if (current_command == 0x0021) rows = 0;
    } else if (current_command == 0x0021 && arg_count == 5) {
        assert(rows < image_height);
        const unsigned width = image_format == 0x0030 ? image_width * 8U : image_width;
        const unsigned x0 = image_format == 0x0030 ? image_x * 8U : image_x;
        assert(size == (image_format == 0x0030 ? width / 8U : width / 2U));
        for (unsigned x = 0; x < width; x++) {
            uint8_t gray;
            if (image_format == 0x0030) {
                /* Words arrive high byte first, pixel 0 is word bit 0. */
                const unsigned group = x / 16U;
                const uint16_t bits = ((uint16_t)tx[group*2] << 8) | tx[group*2+1];
                gray = bits & (1U << (x & 15U)) ? 255 : 0;
            } else {
                assert(image_format == 0x0120);
                gray = (uint8_t)((x & 1U ? tx[x/2] & 15U : tx[x/2] >> 4) * 17U);
            }
            uploaded[(size_t)(image_y+rows)*WIDTH+x0+x] = gray;
        }
        rows++; upload_bytes += size;
    } else {
        assert(size == 2 && arg_count < 8);
        args[arg_count++] = (uint16_t)(((unsigned)tx[0] << 8) | tx[1]);
        if (current_command == 0x0010) last_reg = args[0];
        if (current_command == 0x0011 && arg_count == 2) registers[args[0]] = args[1];
        if (current_command == 0x0021 && arg_count == 5) {
            image_format = args[0]; image_x = args[1]; image_y = args[2];
            image_width = args[3]; image_height = args[4]; upload_bytes = 0;
            assert(image_format == 0x0030 || image_format == 0x0120);
            const unsigned x = image_format == 0x0030 ? image_x*8U : image_x;
            const unsigned w = image_format == 0x0030 ? image_width*8U : image_width;
            assert(!(x & 31U) && !(w & 15U) && w && image_height);
            assert(x + w <= WIDTH && image_y + image_height <= HEIGHT);
        }
        if (current_command == 0x0039 && arg_count == 2) assert(args[0] == 2 && args[1] == 1400);
        if (current_command == 0x0040 && arg_count == 2) {
            assert(args[0] == 1 && args[1] <= 50);
            temperature_writes++; commanded_temperature = args[1];
        }
        if (current_command == 0x0034 && arg_count == 5) {
            const bool binary = image_format == 0x0030;
            assert(rows == image_height && args[0] == image_x * (binary ? 8U : 1U));
            assert(args[1] == image_y && args[2] == image_width * (binary ? 8U : 1U));
            assert(args[3] == image_height);
            assert(!!(registers[0x113a] & 4U) == binary);
            if (binary) assert(registers[0x1250] == 0x00ff);
            updates++; last_waveform = args[4];
        }
    }
    return ESP_OK;
}

static solar_os_it8951_config_t config(void)
{
    return (solar_os_it8951_config_t){.spi_bus="spi0", .cs=10, .reset=12, .busy=13,
        .power=11, .ite_power=21, .clock_hz=10000000, .rotation=0, .vcom_mv=1400,
        .read_temperature=sample_temperature};
}

static void test_upload_packing(solar_os_it8951_t *d)
{
    for (unsigned bits = 0; bits < 256; bits++) {
        uint8_t source[4];
        for (unsigned i = 0; i < 4; i++) {
            source[i] = (bits & (0x80U >> (2U*i)) ? 0xf0 : 0) |
                (bits & (0x40U >> (2U*i)) ? 0x0f : 0);
        }
        assert(pack_binary_byte(source) == bits);
    }
    const dirty_area_t full = {0,WIDTH,0,HEIGHT};
    for (unsigned binary = 0; binary < 2; binary++) {
        uint32_t random = 42;
        for (size_t i = 0; i < GRAY_BYTES; i++) {
            random = random*1664525U + 1013904223U;
            d->gray[i] = binary ? ((random & 0x80000000U ? 0xf0 : 0) |
                (random & 0x40000000U ? 0x0f : 0)) : (uint8_t)(random >> 24);
        }
        assert(finish_refresh(d,binary,&full) == ESP_OK);
        assert(upload_bytes == (size_t)WIDTH*HEIGHT/(binary ? 8U : 2U));
        for (unsigned y = 0; y < HEIGHT; y++)
            for (unsigned x = 0; x < WIDTH; x++)
                assert(uploaded[(size_t)y*WIDTH+x] == gray_at(d->gray,WIDTH-1U-x,y)*17U);
    }
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
    const uint8_t active = gray_at(d->gray,0,0);
    assert(solar_os_it8951_present_frame(d,&patch)==ESP_OK);
    palette=0x4208; patch.x=WIDTH-1; patch.y=HEIGHT-1;
    assert(solar_os_it8951_present_frame(d,&patch)==ESP_OK);
    palette=0xffff; patch.x=patch.y=0;
    assert(solar_os_it8951_present_frame(d,&patch)==ESP_OK);
    assert(updates==before && gray_at(d->gray,0,0)==active);
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
    fail_after=2; pump(&d->scan_worker);
    assert(d->ready && d->shadow_valid && gray_at(d->gray,0,0)==0);
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

int main(void)
{
    solar_os_it8951_t d;
    solar_os_it8951_config_t cfg = config();
    cfg.vcom_mv = 2330; assert(solar_os_it8951_init(&d, &cfg) == ESP_ERR_INVALID_ARG);
    cfg = config(); cfg.ite_power = cfg.cs;
    assert(solar_os_it8951_init(&d, &cfg) == ESP_ERR_INVALID_ARG);
    cfg = config(); cfg.clock_hz = 20000000;
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
    assert(solar_os_it8951_init(&d, &cfg) == ESP_OK);
    assert(d.ready && d.image_address == 0x123456 && !updates);
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
    assert(solar_os_it8951_set_mode(&d,"refresh=full") == ESP_OK);
    assert(solar_os_it8951_present_sync(&d) == ESP_OK && last_waveform == 2);
    assert(solar_os_it8951_set_mode(&d,"refresh=auto") == ESP_OK);
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
    test_upload_packing(&d);
    test_read_recovery(&d);
    solar_os_it8951_deinit(&d);
    assert(!held && !selected && !d.spi && removed == 4);
    cfg = config(); cfg.clock_hz = 5000000;
    assert(solar_os_it8951_init(&d,&cfg) == ESP_OK);
    assert(d.spi_clock_khz == 5000 && strstr(solar_os_it8951_mode(&d),"spi=5000kHz"));
    solar_os_it8951_deinit(&d);
    puts("IT8951 protocol, framebuffer, grayscale, rotation and recovery tests passed");
    return 0;
}

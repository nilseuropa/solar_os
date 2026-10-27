#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_display_link_transport.h"
#include "solar_os_dlp_receiver.h"
#include "solar_os_memory.h"
#include "esp_log.h"
#undef ESP_LOGW
#define ESP_LOGW(tag, ...) ((void)(tag), (void)snprintf(NULL, 0, __VA_ARGS__))
#include "solar_os_port.h"
#include <freertos/semphr.h>
#include "freertos/task.h"

static solar_os_dlp_receiver_t receiver;
static uint8_t images[52], shown[26], input[16384], output[16384];
static size_t in_at, in_size, out_at, out_size;
static uint64_t clock_us;
static unsigned presented, allocations, opens, closes;
static bool fail_write, cancel, legacy_only, silent_peer;
static uint64_t cancel_at;
static bool fail_cache;
static unsigned transient_reads, transient_writes;
static bool graphics;
static const char *fail_graphics_tag;
uint32_t esp_random(void) { return 1000 + opens; }
int64_t esp_timer_get_time(void) { return clock_us; }
uint64_t solar_os_time_uptime_ms(void) { return clock_us / 1000; }
esp_err_t solar_os_time_get_utc_epoch_ms(uint64_t *v) { *v = 0; return ESP_OK; }
void vTaskDelay(TickType_t ticks) { clock_us += (uint64_t)ticks * 1000; }
size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t n = strlen(src), copied = n < size ? n : size - 1;
    if (size) { memcpy(dst, src, copied); dst[copied] = 0; } return n;
}
void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t cls, const char *tag) {
    (void)cls;
    assert(graphics || (strcmp(tag, "display.link.mono") && strcmp(tag, "display.link.rgb")));
    if (fail_graphics_tag && !strcmp(tag, fail_graphics_tag)) return NULL;
    if (fail_cache && (!strcmp(tag, "display.link.wire") || !strcmp(tag, "display.link.retained")))
        return NULL;
    void *p = malloc(size); if (p) allocations++; return p;
}
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t cls, const char *tag) {
    void *p = solar_os_memory_alloc(n * size, cls, tag); if (p) memset(p, 0, n * size); return p;
}
void solar_os_memory_free(void *p) { if (p) { assert(allocations); allocations--; free(p); } }
static int read_device(void *ctx, uint8_t *data, size_t size) {
    (void)ctx;
    size_t n = in_size - in_at; if (n > size) n = size; if (n > 7) n = 7;
    memcpy(data, input + in_at, n); in_at += n; return n;
}
static int write_device(void *ctx, const uint8_t *data, size_t size) {
    (void)ctx;
    if (size > 3) size = 3;
    assert(out_size + size <= sizeof(output));
    memcpy(output + out_size, data, size); out_size += size; return size;
}
static int present_device(void *ctx, unsigned format, const uint8_t *data,
    size_t size, uint16_t width, uint16_t height) {
    (void)ctx;
    assert(format == SDLP_MONO1 && size == 26 && width == 13 && height == 13);
    memcpy(shown, data, size); presented++; return SDLP_OK;
}
static esp_err_t open_port(void *ctx) {
    (void)ctx; opens++;
    in_at = in_size = out_at = out_size = 0;
    solar_os_dlp_receiver_reset(&receiver, receiver.device.boot + 1);
    return ESP_OK;
}
static esp_err_t close_port(void *ctx) { (void)ctx; closes++; return ESP_OK; }
static esp_err_t read_port(void *ctx, uint8_t *data, size_t size, uint32_t timeout, size_t *count) {
    (void)ctx; (void)timeout;
    *count = 0;
    if (silent_peer) return ESP_ERR_TIMEOUT;
    if (transient_reads) { transient_reads--; return ESP_FAIL; }
    assert(!solar_os_dlp_receiver_pump(&receiver, clock_us));
    size_t n = out_size - out_at; if (n > size) n = size; if (n > 5) n = 5;
    memcpy(data, output + out_at, n); out_at += n; *count = n;
    return n ? ESP_OK : ESP_ERR_TIMEOUT;
}
static esp_err_t write_port(void *ctx, const uint8_t *data, size_t size, size_t *count) {
    (void)ctx; *count = 0;
    if (fail_write) return ESP_FAIL;
    if (transient_writes) { transient_writes--; return ESP_FAIL; }
    /* Simulate a legacy-only peer that ignores compact framing completely. */
    if (legacy_only && size >= 4 && !memcmp(data, "DLP1", 4)) { *count = size; return ESP_OK; }
    if (size > 11) size = 11;
    if (in_at == in_size) in_at = in_size = 0;
    if (out_at == out_size) out_at = out_size = 0;
    assert(in_size + size <= sizeof(input));
    memcpy(input + in_size, data, size); in_size += size; *count = size;
    return ESP_OK;
}
static bool should_cancel(void *ctx) { (void)ctx; return cancel || (cancel_at && clock_us >= cancel_at); }
static const char *TAG = "dlp-host";
#include "dlp_client_core.inc"

int main(void) {
    sdlp_info_t info = {.width = 13, .height = 13, .formats = 1,
        .preferred_format = SDLP_MONO1, .max_rect_bytes = 26, .command_ms = 1000,
        .present_ms = 1000, .capabilities = SDLP_RLE | SDLP_SYNC | SDLP_PRESERVE_CONTENT};
    solar_os_dlp_receiver_io_t io = {.read = read_device, .write = write_device, .present = present_device};
    assert(solar_os_dlp_receiver_init(&receiver, &info, 42, images, 26, &io));
    solar_os_port_driver_t port = {.name = "test0", .label = "test", .open = open_port,
        .close = close_port, .read = read_port, .write = write_port,
        .capabilities = SOLAR_OS_PORT_CAP_READ | SOLAR_OS_PORT_CAP_WRITE};
    assert(solar_os_port_register(&port) == ESP_OK);
    for (unsigned mode = 0; mode < 3; mode++) {
        legacy_only = mode == 1; fail_cache = mode == 2;
        solar_os_display_link_t *link = NULL, *busy = NULL;
        solar_os_display_link_options_t options = {.format = SDLP_MONO1, .timeout_ms = 100,
            .cancelled = should_cancel};
        assert(solar_os_display_link_open_stream("test0", "test", &options, &link) == ESP_OK);
        assert(link && !link->mono && !link->pixels);
        assert(solar_os_display_link_info(link)->width == 13);
        assert(solar_os_display_link_format(link) == SDLP_MONO1);
        assert(solar_os_display_link_open_stream("test0", "busy", &options, &busy) == ESP_ERR_INVALID_STATE);
        assert(!busy);
        uint8_t pixels[26] = {0};
        unsigned before = presented;
        assert(solar_os_display_link_present(link, pixels, sizeof(pixels) - 1) == ESP_ERR_INVALID_SIZE);
        assert(presented == before);
        assert(solar_os_display_link_present(link, pixels, sizeof(pixels)) == ESP_OK);
        assert(presented == before + 1 && !memcmp(shown, pixels, sizeof(pixels)));
        assert(solar_os_display_link_present(link, pixels, sizeof(pixels)) == ESP_OK);
        assert(presented == before + (fail_cache ? 2 : 1));
        pixels[4] = 5;
        fail_write = true;
        assert(solar_os_display_link_present(link, pixels, sizeof(pixels)) != ESP_OK);
        fail_write = false;
        assert(solar_os_display_link_present(link, pixels, sizeof(pixels)) == ESP_OK);
        assert(!memcmp(shown, pixels, sizeof(pixels)));
        solar_os_display_link_destroy(link);
        assert(!allocations && opens == closes);
    }
    legacy_only = fail_cache = false;
    solar_os_display_link_t *link = NULL;
    solar_os_display_link_options_t options = {.format = SDLP_MONO1, .timeout_ms = 100,
        .cancelled = should_cancel};
    options.timeout_ms = 500;
    transient_reads = transient_writes = 1;
    unsigned before_opens = opens;
    assert(solar_os_display_link_open_stream("test0", "test", &options, &link) == ESP_OK);
    assert(opens == before_opens + 3 && !transient_reads && !transient_writes);
    assert(solar_os_display_link_present(link, shown, sizeof(shown)) == ESP_OK);
    solar_os_display_link_destroy(link);
    assert(!allocations && opens == closes);
    link = NULL;
    options.timeout_ms = 100;
    /* An open stream with no responding DLP peripheral must yield, time out,
     * and release ownership just like an explicit transport failure. */
    silent_peer = true;
    uint64_t started = clock_us;
    assert(solar_os_display_link_open_stream("test0", "test", &options, &link) != ESP_OK);
    assert(!link && !allocations && opens == closes && clock_us - started <= 120000);
    silent_peer = false;
    fail_write = true;
    started = clock_us;
    assert(solar_os_display_link_open_stream("test0", "test", &options, &link) != ESP_OK);
    assert(!link && !allocations && opens == closes && clock_us - started <= 120000);
    options.timeout_ms = 500;
    cancel_at = clock_us + 25000;
    started = clock_us;
    assert(solar_os_display_link_open_stream("test0", "test", &options, &link) != ESP_OK);
    assert(!link && !allocations && opens == closes && clock_us - started < 100000);
    cancel_at = 0; fail_write = false; options.timeout_ms = 100;
    cancel = true;
    assert(solar_os_display_link_open_stream("test0", "test", &options, &link) != ESP_OK);
    assert(!link && !allocations && opens == closes);
    cancel = false;
    assert(solar_os_display_link_open_stream("test0", "test", &options, &link) == ESP_OK);
    cancel = true;
    assert(solar_os_display_link_present(link, shown, sizeof(shown)) != ESP_OK);
    solar_os_display_link_destroy(link);
    assert(!allocations && opens == closes);
    cancel = false;
    graphics = true;
    for (unsigned mode = 0; mode < 3; mode++) {
        assert(solar_os_display_link_open_stream("test0", "graphics", &options, &link) == ESP_OK);
        unsigned base = allocations;
        fail_graphics_tag = mode == 1 ? "display.link.mono" : mode == 2 ? "display.link.rgb" : NULL;
        if (fail_graphics_tag) {
            assert(!solar_os_display_link_u8g2(link));
            assert(allocations == base && !link->mono && !link->pixels);
            fail_graphics_tag = NULL;
        }
        assert(solar_os_display_link_u8g2(link));
        assert(allocations == base + 2);
        assert(solar_os_display_link_u8g2(link) && allocations == base + 2);
        solar_os_display_link_destroy(link);
        assert(!allocations && opens == closes);
    }
    assert(solar_os_port_unregister("test0") == ESP_OK);
    puts("DLP stream host: compact/legacy, fragmented I/O, delta, cache pressure, reconnect, cancellation, graphics allocation failure and cleanup PASS");
}

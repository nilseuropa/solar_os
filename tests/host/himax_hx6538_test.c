#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include "himax_hx6538.h"
#include "solar_os_json_scan.h"

/* Exercise the firmware's codec boundary with the host's standard decoder. */
int mbedtls_base64_decode(unsigned char *dst, size_t capacity, size_t *length,
                          const unsigned char *source, size_t size)
{
    if (size % 4 || capacity < size / 4 * 3) return -1;
    unsigned char *copy = malloc(size); assert(copy); memcpy(copy, source, size);
    int decoded = EVP_DecodeBlock(dst, copy, (int)size);
    if (decoded >= 0 && size && copy[size - 1] == '=') decoded--;
    if (decoded >= 0 && size > 1 && copy[size - 2] == '=') decoded--;
    free(copy);
    if (decoded < 0) return -1;
    *length = decoded;
    return 0;
}

static const char version[] = "\r{\"type\":0,\"name\":\"VER?\",\"code\":0,\"data\":{\"software\":\"2024.08.16\"}}\n";
static const char sensors[] = "\r{\"type\":0,\"name\":\"SENSORS?\",\"code\":0,\"data\":[{\"id\":9,\"type\":0},{\"id\":1,\"type\":1,\"opts\":{\"0\":\"240x240 Auto\",\"7\":\"416x416 Auto\",\"3\":\"640x480 Auto\"}}]}\n";
static const uint8_t jpeg[] = {0xff,0xd8,0xff,0xc0,0,11,8,1,0xe0,2,0x80,1,1,0x11,0,0xff,0xd9};
static char sample[512];
typedef struct {
    const char *pending;
    size_t position, chunk, allocations, max_allocation;
    uint64_t now;
    bool fail_read, fail_write, fail_resize;
} fake_t;

static esp_err_t write_bytes(void *ctx, const uint8_t *data, size_t size)
{
    fake_t *f = ctx;
    if (f->fail_write) return ESP_FAIL;
    if (size == 9 && !memcmp(data, "AT+VER?\r\n", 9)) f->pending = version;
    else if (size == 13 && !memcmp(data, "AT+SENSORS?\r\n", 13)) f->pending = sensors;
    else if (size == 13 && !memcmp(data, "AT+SAMPLE=1\r\n", 13)) { if (!f->pending) f->pending = sample; }
    f->position = 0;
    return ESP_OK;
}
static esp_err_t read_bytes(void *ctx, uint8_t *data, size_t capacity, size_t *size)
{
    fake_t *f = ctx; *size = 0;
    if (f->fail_read) return ESP_FAIL;
    if (!f->pending) return ESP_OK;
    size_t n = strlen(f->pending + f->position);
    if (n > capacity) n = capacity;
    if (n > f->chunk) n = f->chunk;
    memcpy(data, f->pending + f->position, n); f->position += n; *size = n;
    if (!f->pending[f->position]) f->pending = NULL;
    return ESP_OK;
}
static uint64_t now_ms(void *ctx) { return ((fake_t *)ctx)->now; }
static void wait_ms(void *ctx, uint32_t ms) { ((fake_t *)ctx)->now += ms; }
static void *resize(void *ctx, void *data, size_t size)
{
    fake_t *f = ctx;
    if (!size) { if (data) f->allocations--; free(data); return NULL; }
    if (f->fail_resize) return NULL;
    if (size > f->max_allocation) f->max_allocation = size;
    if (!data) f->allocations++;
    return realloc(data, size);
}

int main(void)
{
    unsigned char encoded[128];
    assert(EVP_EncodeBlock(encoded, jpeg, sizeof(jpeg)) > 0);
    snprintf(sample, sizeof(sample), "\r{\"type\":0,\"name\":\"SAMPLE\",\"code\":0}\n"
        "\r{\"type\":1,\"name\":\"SAMPLE\",\"code\":0,\"data\":{\"image\":\"%s\"}}\n", encoded);
    fake_t fake = {.chunk = 3};
    solar_os_himax_t *camera = calloc(1, sizeof(*camera)); assert(camera);
    const solar_os_himax_io_t io = {.ctx=&fake,.write=write_bytes,.read=read_bytes,
        .now_ms=now_ms,.wait_ms=wait_ms,.resize=resize};
    assert(solar_os_himax_init(camera, &io) == ESP_OK);
    assert(solar_os_himax_probe(camera, 100) == ESP_OK);
    assert(!strcmp(camera->version, "2024.08.16") && camera->mode_count == 3 && camera->sensor_id == 1);
    assert(camera->modes[1].width == 640 && camera->modes[1].height == 480 && camera->modes[1].option == 3);
    assert(camera->modes[2].width == 416 && camera->modes[2].option == 7);
    const uint8_t *image; size_t length; uint16_t width, height;
    for (size_t chunk = 1; chunk <= 4095; chunk = chunk == 1 ? 7 : chunk == 7 ? 4095 : 4096) {
        fake.chunk = chunk;
        assert(solar_os_himax_sample(camera, 100, &image, &length, &width, &height) == ESP_OK);
        assert(length == sizeof(jpeg) && !memcmp(image, jpeg, length) && width == 640 && height == 480);
        solar_os_himax_clear(camera); assert(fake.allocations == 0);
    }
    /* The factory firmware sends zero-filled storage after the JPEG EOI. */
    const size_t padded_size = 24576;
    uint8_t *padded = calloc(1, padded_size); assert(padded);
    unsigned char *large_encoded = malloc(padded_size * 2); assert(large_encoded);
    char *large_reply = malloc(padded_size * 2 + 256); assert(large_reply);
    memcpy(padded, jpeg, sizeof(jpeg));
    assert(EVP_EncodeBlock(large_encoded, padded, padded_size) > 0);
    snprintf(large_reply, padded_size * 2 + 256,
        "\r{\"type\":1,\"name\":\"SAMPLE\",\"code\":0,\"data\":{\"count\":1,\"resolution\":[640,480],\"image\":\"%s\",\"rotate\":0}}\n",
        large_encoded);
    fake.pending = large_reply;
    assert(solar_os_himax_sample(camera, 100, &image, &length, &width, &height) == ESP_OK);
    assert(length == sizeof(jpeg) && !memcmp(image, jpeg, length) && width == 640 && height == 480);
    solar_os_himax_clear(camera);
    free(large_reply); free(large_encoded); free(padded);
    fake.fail_write = true;
    assert(solar_os_himax_sample(camera, 100, &image, &length, &width, &height) == ESP_FAIL && !image);
    fake.fail_write = false; fake.fail_read = true;
    assert(solar_os_himax_sample(camera, 100, &image, &length, &width, &height) == ESP_FAIL);
    fake.fail_read = false; fake.pending = NULL; fake.fail_resize = true;
    assert(solar_os_himax_sample(camera, 100, &image, &length, &width, &height) == ESP_ERR_NO_MEM);
    solar_os_himax_clear(camera); fake.fail_resize = false;
    fake.pending = "\r{\"type\":0,\"name\":\"SAMPLE\",\"code\":8}\n";
    assert(solar_os_himax_sample(camera, 100, &image, &length, &width, &height) == ESP_ERR_NOT_SUPPORTED);
    solar_os_himax_clear(camera);
    fake.pending = "\r{\"type\":1,\"name\":\"SAMPLE\",\"code\":0,\"data\":{\"image\":\"bad!\"}}\n";
    assert(solar_os_himax_sample(camera, 100, &image, &length, &width, &height) == ESP_ERR_INVALID_RESPONSE && !image);
    solar_os_himax_clear(camera);
    fake.pending = "\r{\"type\":1,\"name\":\"SAMPLE\",\"code\":0,\"data\":{\"image\":\"/9j/2Q==\"}}\n";
    assert(solar_os_himax_sample(camera, 100, &image, &length, &width, &height) == ESP_ERR_INVALID_RESPONSE && !image);
    solar_os_himax_clear(camera);
    fake.pending = "unrelated boot log\n";
    assert(solar_os_himax_sample(camera, 10, &image, &length, &width, &height) == ESP_ERR_TIMEOUT);
    solar_os_himax_clear(camera);
    char *oversized = malloc(SOLAR_OS_HIMAX_REPLY_MAX + 8); assert(oversized);
    memset(oversized, 'a', SOLAR_OS_HIMAX_REPLY_MAX + 7); oversized[0]='{'; oversized[SOLAR_OS_HIMAX_REPLY_MAX+7]=0;
    fake.pending = oversized;
    assert(solar_os_himax_sample(camera, 100, &image, &length, &width, &height) == ESP_ERR_INVALID_SIZE);
    assert(fake.max_allocation == SOLAR_OS_HIMAX_REPLY_MAX);
    solar_os_himax_clear(camera); free(oversized); assert(!fake.allocations);
    const char *value; size_t span;
    assert(solar_os_json_scan_object_value("{\"data\":[1,{\"x\":\"a}b\"}]}", "data", &value, &span));
    assert(solar_os_json_scan_array_value(value, 1, &value, &span) && span == 11 && !memcmp(value,"{\"x\":\"a}b\"}",span));
    assert(!solar_os_json_scan_array_value("[1]", 1, &value, &span));
    assert(!solar_os_json_scan_object_value("{\"x\":[1}", "x", &value, &span));
    free(camera);
    puts("Himax sampling, modes, fragmented replies, limits and failure cleanup passed");
}

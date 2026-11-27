#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os.h"
#include "solar_os_memory.h"
#include "solar_os_mjpeg.h"
#include "solar_os_native_mjpeg.h"
#include "solar_os_native_mjpeg_abi.h"

typedef struct {
    uint8_t frames[4][32];
    size_t lengths[4];
    size_t count;
    esp_err_t result;
} capture_t;

static esp_err_t capture_frame(const uint8_t *jpeg, size_t jpeg_len, void *user)
{
    capture_t *capture = user;
    assert(capture != NULL);
    assert(capture->count < 4U);
    assert(jpeg_len <= sizeof(capture->frames[0]));
    memcpy(capture->frames[capture->count], jpeg, jpeg_len);
    capture->lengths[capture->count] = jpeg_len;
    capture->count++;
    return capture->result;
}

static void test_extracts_chunked_frames(void)
{
    uint8_t buffer[32];
    capture_t capture = {0};
    solar_os_mjpeg_parser_t parser;
    assert(solar_os_mjpeg_parser_init(
               &parser, buffer, sizeof(buffer)) == ESP_OK);

    static const uint8_t first[] = {
        '-', '-', 'b', '\r', '\n', 0xff,
    };
    static const uint8_t second[] = {
        0xd8, 0x01, 0xff, 0x00, 0x02, 0xff,
    };
    static const uint8_t third[] = {
        0xd9, '\r', '\n', '-', '-', 'b', '\r', '\n', 0xff, 0xd8, 0x03, 0xff, 0xd9,
    };
    assert(solar_os_mjpeg_parser_feed(&parser, first, sizeof(first), capture_frame, &capture, NULL) == ESP_OK);
    assert(solar_os_mjpeg_parser_feed(&parser, second, sizeof(second), capture_frame, &capture, NULL) == ESP_OK);
    assert(solar_os_mjpeg_parser_feed(&parser, third, sizeof(third), capture_frame, &capture, NULL) == ESP_OK);

    static const uint8_t expected_first[] = {
        0xff, 0xd8, 0x01, 0xff, 0x00, 0x02, 0xff, 0xd9,
    };
    static const uint8_t expected_second[] = {
        0xff, 0xd8, 0x03, 0xff, 0xd9,
    };
    assert(capture.count == 2U);
    assert(capture.lengths[0] == sizeof(expected_first));
    assert(memcmp(capture.frames[0], expected_first, sizeof(expected_first)) == 0);
    assert(capture.lengths[1] == sizeof(expected_second));
    assert(memcmp(capture.frames[1], expected_second, sizeof(expected_second)) == 0);
    assert(parser.frames == 2U);
    assert(parser.dropped_frames == 0U);
}

static void test_drops_oversize_and_recovers(void)
{
    uint8_t buffer[6];
    capture_t capture = {0};
    solar_os_mjpeg_parser_t parser;
    assert(solar_os_mjpeg_parser_init(
               &parser, buffer, sizeof(buffer)) == ESP_OK);

    static const uint8_t stream[] = {
        0xff, 0xd8, 1, 2, 3, 4, 5, 6, 0xff, 0xd9,
        0xff, 0xd8, 7, 0xff, 0xd9,
    };
    assert(solar_os_mjpeg_parser_feed(&parser, stream, sizeof(stream), capture_frame, &capture, NULL) == ESP_OK);
    static const uint8_t expected[] = {0xff, 0xd8, 7, 0xff, 0xd9};
    assert(capture.count == 1U);
    assert(capture.lengths[0] == sizeof(expected));
    assert(memcmp(capture.frames[0], expected, sizeof(expected)) == 0);
    assert(parser.frames == 1U);
    assert(parser.dropped_frames == 1U);
}

static void test_reset_discards_partial_frame(void)
{
    uint8_t buffer[16];
    capture_t capture = {0};
    solar_os_mjpeg_parser_t parser;
    assert(solar_os_mjpeg_parser_init(
               &parser, buffer, sizeof(buffer)) == ESP_OK);

    static const uint8_t partial[] = {0xff, 0xd8, 1, 2};
    static const uint8_t complete[] = {0xff, 0xd8, 3, 0xff, 0xd9};
    assert(solar_os_mjpeg_parser_feed(&parser, partial, sizeof(partial), capture_frame, &capture, NULL) == ESP_OK);
    solar_os_mjpeg_parser_reset(&parser);
    assert(solar_os_mjpeg_parser_feed(&parser, complete, sizeof(complete), capture_frame, &capture, NULL) == ESP_OK);
    assert(capture.count == 1U);
    assert(capture.frames[0][2] == 3U);
}

static void test_propagates_callback_error_after_complete_frame(void)
{
    uint8_t buffer[16];
    capture_t capture = {.result = ESP_FAIL};
    solar_os_mjpeg_parser_t parser;
    assert(solar_os_mjpeg_parser_init(
               &parser, buffer, sizeof(buffer)) == ESP_OK);

    static const uint8_t frame[] = {0xff, 0xd8, 1, 0xff, 0xd9};
    assert(solar_os_mjpeg_parser_feed(&parser, frame, sizeof(frame), capture_frame, &capture, NULL) == ESP_FAIL);
    assert(capture.count == 1U);
    assert(parser.frames == 1U);
    assert(!parser.collecting);
}

static unsigned allocations;
static bool fail_alloc;

void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t cls, const char *tag)
{
    assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && strcmp(tag, "mjpeg.parser") == 0);
    if (fail_alloc) return NULL;
    void *ptr = malloc(size);
    assert(ptr);
    ++allocations;
    return ptr;
}

void solar_os_memory_free(void *ptr)
{
    assert(ptr && allocations);
    --allocations;
    free(ptr);
}

const solar_os_app_t *solar_os_application_descriptor(void)
{
    return NULL;
}

typedef struct {
    const solar_os_native_mjpeg_api_v1_t *api;
    solar_os_native_handle_t parser;
    const uint8_t *buffer;
    uint8_t expected;
    int result;
    unsigned seen;
} native_capture_t;

static int native_capture_frame(const uint8_t *jpeg, size_t length, void *user)
{
    native_capture_t *capture = user;
    assert(jpeg == capture->buffer && length == 5 && jpeg[2] == capture->expected);
    ++capture->seen;
    const solar_os_native_mjpeg_api_v1_t *api = capture->api;
    assert(api->reset(capture->parser) == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(api->close(capture->parser) == SOLAR_OS_NATIVE_ERROR_BUSY);
    return capture->result;
}

static void test_native_parser(void)
{
    const solar_os_native_mjpeg_api_v1_t *api = solar_os_native_mjpeg_get_service(
        SOLAR_OS_NATIVE_MJPEG_SERVICE, SOLAR_OS_NATIVE_MJPEG_ABI, sizeof(*api));
    assert(api && api->struct_size == sizeof(*api));
    uint8_t buffer[8];
    native_capture_t capture = {.api = api, .buffer = buffer, .expected = 1,
        .result = SOLAR_OS_NATIVE_ERROR_INVALID_STATE};
    fail_alloc = true;
    assert(api->open(buffer, sizeof(buffer), &capture.parser) == SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
    assert(!capture.parser && !allocations);
    fail_alloc = false;
    assert(api->open(buffer, sizeof(buffer), &capture.parser) == SOLAR_OS_NATIVE_OK);
    const uint8_t first[] = {0xff};
    const uint8_t next[] = {0xd8, 1, 0xff, 0xd9, 0xff, 0xd8, 2, 0xff, 0xd9};
    size_t consumed = 0;
    assert(api->feed(capture.parser, first, sizeof(first), native_capture_frame, &capture, &consumed)
        == SOLAR_OS_NATIVE_OK);
    assert(consumed == sizeof(first) && !capture.seen);
    assert(api->feed(capture.parser, next, sizeof(next), native_capture_frame, &capture, &consumed)
        == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(consumed == 4 && capture.seen == 1);
    size_t remaining = sizeof(next) - consumed;
    capture.expected = 2;
    capture.result = SOLAR_OS_NATIVE_OK;
    assert(api->feed(capture.parser, next + consumed, remaining, native_capture_frame, &capture, &consumed)
        == SOLAR_OS_NATIVE_OK);
    assert(consumed == remaining && capture.seen == 2);
    assert(api->reset(capture.parser) == SOLAR_OS_NATIVE_OK);
    solar_os_native_mjpeg_status_v1_t status;
    assert(api->status(capture.parser, &status) == SOLAR_OS_NATIVE_OK);
    assert(status.frames == 2 && !status.dropped_frames);
    assert(api->close(capture.parser) == SOLAR_OS_NATIVE_OK && !allocations);
    assert(api->reset(capture.parser) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
}

int main(void)
{
    test_extracts_chunked_frames();
    test_drops_oversize_and_recovers();
    test_reset_discards_partial_frame();
    test_propagates_callback_error_after_complete_frame();
    test_native_parser();
    puts("MJPEG parser and ABI tests: ok");
    return 0;
}

#include "solar_os_mjpeg.h"

#include <string.h>

#define JPEG_MARKER_PREFIX 0xffU
#define JPEG_MARKER_SOI 0xd8U
#define JPEG_MARKER_EOI 0xd9U

static void mjpeg_seek(solar_os_mjpeg_parser_t *parser, uint8_t byte)
{
    if (parser->seek_ff && byte == JPEG_MARKER_SOI) {
        parser->frame[0] = JPEG_MARKER_PREFIX;
        parser->frame[1] = JPEG_MARKER_SOI;
        parser->length = 2U;
        parser->collecting = true;
        parser->seek_ff = false;
        parser->frame_ff = false;
        return;
    }
    parser->seek_ff = byte == JPEG_MARKER_PREFIX;
}

static void mjpeg_drop_frame(solar_os_mjpeg_parser_t *parser, uint8_t byte)
{
    parser->length = 0U;
    parser->collecting = false;
    parser->frame_ff = false;
    parser->seek_ff = byte == JPEG_MARKER_PREFIX;
    parser->dropped_frames++;
}

esp_err_t solar_os_mjpeg_parser_init(solar_os_mjpeg_parser_t *parser,
                                     uint8_t *frame,
                                     size_t capacity)
{
    if (parser == NULL || frame == NULL || capacity < 4U) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(parser, 0, sizeof(*parser));
    parser->frame = frame;
    parser->capacity = capacity;
    return ESP_OK;
}

void solar_os_mjpeg_parser_reset(solar_os_mjpeg_parser_t *parser)
{
    if (parser == NULL) {
        return;
    }
    parser->length = 0U;
    parser->collecting = false;
    parser->seek_ff = false;
    parser->frame_ff = false;
}

esp_err_t solar_os_mjpeg_parser_feed(solar_os_mjpeg_parser_t *parser,
                                     const uint8_t *data,
                                     size_t len,
                                     solar_os_mjpeg_frame_fn on_frame,
                                     void *user,
                                     size_t *consumed)
{
    if (consumed) *consumed = 0;
    if (parser == NULL || parser->frame == NULL || parser->capacity < 4U ||
        on_frame == NULL || (data == NULL && len > 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0U; i < len; i++) {
        const uint8_t byte = data[i];
        if (!parser->collecting) {
            mjpeg_seek(parser, byte);
            continue;
        }
        if (parser->length >= parser->capacity) {
            mjpeg_drop_frame(parser, byte);
            continue;
        }

        parser->frame[parser->length++] = byte;
        if (parser->frame_ff && byte == JPEG_MARKER_EOI) {
            const size_t frame_len = parser->length;
            parser->length = 0U;
            parser->collecting = false;
            parser->frame_ff = false;
            parser->seek_ff = false;
            parser->frames++;
            const esp_err_t error = on_frame(parser->frame, frame_len, user);
            if (error != ESP_OK) {
                if (consumed) *consumed = i + 1;
                return error;
            }
            continue;
        }
        parser->frame_ff = byte == JPEG_MARKER_PREFIX;
    }
    if (consumed) *consumed = len;
    return ESP_OK;
}

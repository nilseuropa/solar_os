#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef esp_err_t (*solar_os_mjpeg_frame_fn)(const uint8_t *jpeg,
                                             size_t jpeg_len,
                                             void *user);

typedef struct {
    uint8_t *frame;
    size_t capacity;
    size_t length;
    bool collecting;
    bool seek_ff;
    bool frame_ff;
    uint32_t frames;
    uint32_t dropped_frames;
} solar_os_mjpeg_parser_t;

esp_err_t solar_os_mjpeg_parser_init(solar_os_mjpeg_parser_t *parser,
                                     uint8_t *frame,
                                     size_t capacity);
void solar_os_mjpeg_parser_reset(solar_os_mjpeg_parser_t *parser);
esp_err_t solar_os_mjpeg_parser_feed(solar_os_mjpeg_parser_t *parser,
                                     const uint8_t *data,
                                     size_t len,
                                     solar_os_mjpeg_frame_fn on_frame,
                                     void *user,
                                     size_t *consumed);

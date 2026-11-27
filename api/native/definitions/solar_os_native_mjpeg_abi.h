#pragma once

#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_MJPEG_SERVICE "media.mjpeg"
#define SOLAR_OS_NATIVE_MJPEG_ABI 1U

typedef int (*solar_os_native_mjpeg_frame_fn)(const uint8_t *jpeg, size_t length, void *user);

typedef struct {
    uint32_t frames, dropped_frames;
} solar_os_native_mjpeg_status_v1_t;

/* Feed successive chunks of an MJPEG stream to a parser. The caller keeps
 * the writable frame buffer live and reserves it for the parser until close.
 * A visitor borrows the complete JPEG bytes only during its call. The parser
 * keeps neither the visitor nor its user pointer. Input chunks must not
 * overlap the frame buffer. If a visitor stops parsing, feed returns that
 * result and reports the consumed prefix, including that frame. Reset drops
 * a partial frame and preserves counters. The parser serializes operations:
 * reentering one from a visitor returns BUSY. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*open)(uint8_t *buffer, size_t capacity, solar_os_native_handle_t *parser);
    int (*feed)(solar_os_native_handle_t parser, const uint8_t *data, size_t length,
        solar_os_native_mjpeg_frame_fn visit, void *user, size_t *consumed);
    int (*reset)(solar_os_native_handle_t parser);
    int (*status)(solar_os_native_handle_t parser, solar_os_native_mjpeg_status_v1_t *status);
    int (*close)(solar_os_native_handle_t parser);
} solar_os_native_mjpeg_api_v1_t;

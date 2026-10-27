#pragma once

#include "dlp.h"

/* Caller-owned receiver, storage and I/O. Pump from the display session owner;
 * callbacks and lifecycle operations must be serialized. No worker is created. */
typedef struct {
    void *user;
    uint32_t packet_idle_us; /* zero selects 1 second; tune for slow transports */
    int (*read)(void *, uint8_t *, size_t); /* bytes, zero=idle, negative=failure */
    int (*write)(void *, const uint8_t *, size_t);
    int (*present)(void *, unsigned, const uint8_t *, size_t, uint16_t, uint16_t);
} solar_os_dlp_receiver_io_t;

typedef struct {
    dlp_device_t device;
    dlp_device_window_t window;
    dlp_stream_t parser;
    solar_os_dlp_receiver_io_t io;
    uint8_t *front, *back;
    size_t capacity, image_size;
    uint16_t x, y, width, height;
    uint32_t stride, rect_stride;
    unsigned format;
    bool pending, valid, reply_compact;
    uint8_t rx[DLP_CELL_SIZE], tx[DLP_CELL_SIZE];
    size_t rx_at, rx_size, tx_at, tx_size;
    uint64_t last_rx_us, last_tx_us;
    uint32_t received_bytes, sent_bytes, presented_frames;
} solar_os_dlp_receiver_t;

/* Storage contains two equally sized images; capacity is bytes per image.
 * MONO1 is mandatory. RGB565_BE may also be advertised. */
bool solar_os_dlp_receiver_init(solar_os_dlp_receiver_t *receiver,
    const dlp_info_t *info, uint64_t boot, uint8_t *storage, size_t capacity,
    const solar_os_dlp_receiver_io_t *io);
/* Fence a disconnected transport with a fresh boot ID. Discard partial work and
 * replies while retaining the last completed image and cumulative statistics. */
void solar_os_dlp_receiver_reset(solar_os_dlp_receiver_t *receiver, uint64_t boot);
/* Bounded protocol work; preserves unread input and partially written replies.
 * Returns a DLP result. Presentation runs here, outside command callbacks. */
int solar_os_dlp_receiver_pump(solar_os_dlp_receiver_t *receiver, uint64_t now_us);

#include "solar_os_dlp_receiver.h"

#include <string.h>

static int configure(void *user, unsigned format)
{
    solar_os_dlp_receiver_t *r = user;
    if (format != DLP_MONO1 && format != DLP_RGB565_BE) return DLP_UNSUPPORTED;
    uint32_t stride = dlp_row_bytes(r->device.info.width, format);
    size_t size = (size_t)stride * r->device.info.height;
    if (size > r->capacity) return DLP_NO_MEMORY;
    r->format = format;
    r->stride = stride;
    r->image_size = size;
    r->pending = r->valid = false;
    return DLP_OK;
}

static int begin_frame(void *user, uint32_t frame, bool full)
{
    (void)frame;
    solar_os_dlp_receiver_t *r = user;
    if (!full && !r->valid) return DLP_BAD_STATE;
    if (full) memset(r->back, r->format == DLP_MONO1 ? 0 : 255, r->image_size);
    else memcpy(r->back, r->front, r->image_size);
    return DLP_OK;
}

static int begin_rect(void *user, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    solar_os_dlp_receiver_t *r = user;
    r->x = x; r->y = y; r->width = w; r->height = h;
    r->rect_stride = dlp_row_bytes(w, r->format);
    return DLP_OK;
}

static int write_rect(void *user, uint32_t offset, const uint8_t *data, size_t size)
{
    solar_os_dlp_receiver_t *r = user;
    /* The protocol validates bounds and row padding before calling us. Rectangles
     * have their own packed stride and may start at any pixel, including x%8. */
    for (size_t i = 0; i < size; i++) {
        uint32_t pos = offset + (uint32_t)i;
        uint32_t row = pos / r->rect_stride, col = pos % r->rect_stride;
        if (r->format == DLP_RGB565_BE) {
            r->back[(size_t)(r->y + row) * r->stride + r->x * 2U + col] = data[i];
        } else {
            for (unsigned bit = 0; bit < 8 && col * 8U + bit < r->width; bit++) {
                unsigned x = r->x + col * 8U + bit;
                uint8_t *dst = &r->back[(size_t)(r->y + row) * r->stride + x / 8U];
                uint8_t mask = (uint8_t)(1U << (x % 8U));
                *dst = (uint8_t)((*dst & ~mask) | ((data[i] & (1U << bit)) ? mask : 0));
            }
        }
    }
    return DLP_OK;
}

static int present(void *user)
{
    ((solar_os_dlp_receiver_t *)user)->pending = true;
    return DLP_OK;
}

static int poll_present(void *user)
{
    solar_os_dlp_receiver_t *r = user;
    if (!r->pending) return DLP_OK;
    int result = r->io.present(r->io.user, r->format, r->back, r->image_size,
        r->device.info.width, r->device.info.height);
    if (result == DLP_BUSY) return result;
    r->pending = false;
    if (result == DLP_OK) {
        uint8_t *old = r->front; r->front = r->back; r->back = old;
        r->valid = true;
        r->presented_frames++;
    }
    return result;
}

static int abort_frame(void *user)
{
    ((solar_os_dlp_receiver_t *)user)->pending = false;
    return DLP_OK;
}

bool solar_os_dlp_receiver_init(solar_os_dlp_receiver_t *r,
    const dlp_info_t *info, uint64_t boot, uint8_t *storage, size_t capacity,
    const solar_os_dlp_receiver_io_t *io)
{
    if (!r || !info || !storage || !capacity || !io || !io->read || !io->write ||
        !io->present || !(info->formats & DLP_FORMAT_BIT(DLP_MONO1)) ||
        (info->formats & ~(DLP_FORMAT_BIT(DLP_MONO1) | DLP_FORMAT_BIT(DLP_RGB565_BE))))
        return false;
    for (unsigned f = 0; f < 5; f++)
        if ((info->formats & DLP_FORMAT_BIT(f)) &&
            (uint64_t)dlp_row_bytes(info->width, f) * info->height > capacity)
            return false;
    memset(r, 0, sizeof(*r));
    r->io = *io; r->front = storage; r->back = storage + capacity; r->capacity = capacity;
    dlp_backend_t backend = {.ctx = r, .configure = configure, .begin_frame = begin_frame,
        .begin_rect = begin_rect, .write = write_rect, .present = present,
        .poll_present = poll_present, .abort = abort_frame};
    return dlp_device_init(&r->device, info, boot, &backend) &&
        dlp_device_enable_window(&r->device, &r->window, DLP_MAX_WINDOW);
}

void solar_os_dlp_receiver_reset(solar_os_dlp_receiver_t *r, uint64_t boot)
{
    dlp_info_t info = r->device.info;
    dlp_backend_t backend = r->device.backend;
    (void)dlp_device_init(&r->device, &info, boot, &backend);
    (void)dlp_device_enable_window(&r->device, &r->window, DLP_MAX_WINDOW);
    memset(&r->parser, 0, sizeof(r->parser));
    r->pending = r->reply_compact = false;
    r->rx_at = r->rx_size = r->tx_at = r->tx_size = 0;
    r->last_rx_us = r->last_tx_us = 0;
}

int solar_os_dlp_receiver_pump(solar_os_dlp_receiver_t *r, uint64_t now_us)
{
    dlp_device_tick(&r->device, now_us);
    uint32_t idle_us = r->io.packet_idle_us ? r->io.packet_idle_us : 1000000U;
    if (r->parser.used && now_us - r->last_rx_us > idle_us) r->parser.used = 0;
    for (unsigned work = 0; work < 16; work++) {
        if (r->tx_at < r->tx_size) {
            int n = r->io.write(r->io.user, r->tx + r->tx_at, r->tx_size - r->tx_at);
            if (n < 0 || (size_t)n > r->tx_size - r->tx_at) return DLP_IO_ERROR;
            if (!n) return now_us - r->last_tx_us > UINT64_C(5000000) ? DLP_TIMEOUT : DLP_OK;
            r->tx_at += (size_t)n; r->sent_bytes += (uint32_t)n;
            r->last_tx_us = now_us;
            if (r->tx_at < r->tx_size) continue;
        }
        uint8_t cell[DLP_CELL_SIZE], response[DLP_CELL_SIZE];
        if (dlp_device_take_reply(&r->device, response)) {
            r->tx_at = 0;
            r->tx_size = dlp_stream_encode(r->tx, response, r->reply_compact);
            r->last_tx_us = now_us;
            continue;
        }
        bool compact = false;
        bool ready = dlp_stream_next(&r->parser, cell, &compact);
        if (!ready && r->rx_at == r->rx_size) {
            int n = r->io.read(r->io.user, r->rx, sizeof(r->rx));
            if (n < 0 || (size_t)n > sizeof(r->rx)) return DLP_IO_ERROR;
            if (!n) break;
            r->rx_at = 0; r->rx_size = (size_t)n;
            r->received_bytes += (uint32_t)n;
            r->last_rx_us = now_us;
        }
        while (!ready && r->rx_at < r->rx_size)
            ready = dlp_stream_feed(&r->parser, r->rx[r->rx_at++], cell, &compact);
        if (ready) {
            if (dlp_device_receive(&r->device, cell, response, now_us)) {
                r->tx_at = 0;
                r->tx_size = dlp_stream_encode(r->tx, response, compact);
                r->last_tx_us = now_us;
            } else if (r->window.waiting &&
                !memcmp(cell, r->window.pending.request, DLP_CELL_SIZE))
                r->reply_compact = compact;
        }
    }
    return DLP_OK;
}

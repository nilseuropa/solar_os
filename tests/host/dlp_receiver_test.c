#include "solar_os_dlp_receiver.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    solar_os_dlp_receiver_t receiver;
    uint8_t storage[2 * 13 * 9 * 2];
    uint8_t input[1024], output[1024], shown[13 * 9 * 2];
    size_t in_at, in_size, out_size, shown_size;
    uint64_t now, busy_until;
    unsigned presents, writes;
    bool legacy, stall;
    int present_result;
} fixture_t;

static int read_bytes(void *user, uint8_t *data, size_t size)
{
    fixture_t *f = user;
    size_t n = f->in_size - f->in_at;
    if (n > size) n = size;
    if (n > 7) n = 7;
    memcpy(data, f->input + f->in_at, n);
    f->in_at += n;
    return (int)n;
}

static int write_bytes(void *user, const uint8_t *data, size_t size)
{
    fixture_t *f = user;
    if (f->stall || ++f->writes % 3 == 0) return 0;
    if (size > 3) size = 3;
    assert(f->out_size + size <= sizeof(f->output));
    memcpy(f->output + f->out_size, data, size);
    f->out_size += size;
    return (int)size;
}

static int present_image(void *user, unsigned format, const uint8_t *data,
    size_t size, uint16_t w, uint16_t h)
{
    fixture_t *f = user;
    assert(w == 13 && h == 9 && (format == DLP_MONO1 || format == DLP_RGB565_BE));
    if (f->now < f->busy_until) return DLP_BUSY;
    if (f->present_result) return f->present_result;
    memcpy(f->shown, data, size); f->shown_size = size; f->presents++;
    return DLP_OK;
}

static uint64_t now(void *user) { return ((fixture_t *)user)->now; }
static void sleep_us(void *user, uint32_t n) { ((fixture_t *)user)->now += n; }

static int request(void *user, const uint8_t tx[256], uint8_t rx[256], uint64_t deadline)
{
    fixture_t *f = user;
    assert(f->in_at == f->in_size);
    f->in_at = 0;
    f->in_size = dlp_stream_encode(f->input, tx, !f->legacy);
    f->out_size = 0;
    dlp_stream_t parser = {0};
    size_t at = 0;
    while (f->now < deadline) {
        f->now += 1000;
        int result = solar_os_dlp_receiver_pump(&f->receiver, f->now);
        if (result) return DLP_LINK_ERROR;
        while (at < f->out_size) {
            bool compact;
            if (dlp_stream_feed(&parser, f->output[at++], rx, &compact)) {
                assert(compact == !f->legacy);
                return DLP_OK;
            }
        }
    }
    return DLP_DEADLINE;
}

static int push(void *user, const uint8_t tx[256], uint64_t deadline)
{
    fixture_t *f = user;
    assert(f->in_at == f->in_size && !f->out_size);
    f->in_at = 0;
    f->in_size = dlp_stream_encode(f->input, tx, !f->legacy);
    while (f->in_at < f->in_size && f->now < deadline) {
        f->now += 1000;
        if (solar_os_dlp_receiver_pump(&f->receiver, f->now)) return DLP_LINK_ERROR;
    }
    return f->in_at == f->in_size ? DLP_OK : DLP_DEADLINE;
}

static void setup(fixture_t *f, dlp_host_t *host, bool legacy)
{
    memset(f, 0, sizeof(*f));
    f->legacy = legacy;
    dlp_info_t info = {.width = 13, .height = 9, .formats = 17,
        .preferred_format = DLP_MONO1, .max_rect_bytes = sizeof(f->shown),
        .capabilities = DLP_PRESERVE_CONTENT | DLP_STAGED_FRAME | DLP_RLE | DLP_SYNC,
        .command_ms = 1000, .present_ms = 1000, .nominal_refresh_ms = 1};
    solar_os_dlp_receiver_io_t io = {.user = f, .read = read_bytes,
        .write = write_bytes, .present = present_image};
    assert(solar_os_dlp_receiver_init(&f->receiver, &info, 42, f->storage,
        sizeof(f->storage) / 2, &io));
    dlp_transport_t transport = {.ctx = f, .now_us = now, .sleep_us = sleep_us, .request = request};
    dlp_host_init(host, &transport);
    assert(dlp_host_discover(host, 1000000) == DLP_OK);
    assert(dlp_host_configure(host, 1, DLP_MONO1, 0) == DLP_OK);
    assert(dlp_host_extensions(host) == DLP_OK);
}

static void exercise(bool legacy)
{
    fixture_t f;
    dlp_host_t host;
    setup(&f, &host, legacy);
    uint8_t image[18] = {0}, retained[18] = {0};
    bool valid = false;
    assert(dlp_host_upload_frame(&host, 1, image, sizeof(image), retained, &valid) == DLP_OK);
    assert(valid && f.presents == 1 && f.shown_size == sizeof(image));
    assert(!memcmp(image, f.shown, sizeof(image)));
    assert(dlp_host_upload_frame(&host, 2, image, sizeof(image), retained, &valid) == DLP_OK);
    assert(f.presents == 1); /* unchanged image confirmed by GET_SYNC */
    image[8] = 0xa5;
    assert(dlp_host_upload_frame(&host, 2, image, sizeof(image), retained, &valid) == DLP_OK);
    assert(f.presents == 2 && !memcmp(image, f.shown, sizeof(image)));

    /* Arbitrarily aligned rectangles, split writes and independent row padding. */
    assert(dlp_host_begin_frame(&host, 3, false) == DLP_OK);
    assert(dlp_host_begin_rect(&host, 3, 1, 3, 1, 9, 2) == DLP_OK);
    uint8_t rect[] = {0x15, 1, 0x55, 0};
    assert(dlp_host_write(&host, 3, 1, 0, rect, 3) == DLP_OK);
    assert(dlp_host_write(&host, 3, 1, 3, rect + 3, 1) == DLP_OK);
    assert(f.presents == 2 && !memcmp(image, f.shown, sizeof(image)));
    assert(dlp_host_present(&host, 3) == DLP_OK);
    uint32_t sequence = host.sequence;
    for (unsigned y = 0; y < 2; y++) for (unsigned x = 0; x < 9; x++) {
        unsigned dst = (y + 1) * 2 + (x + 3) / 8;
        unsigned mask = 1U << ((x + 3) % 8);
        image[dst] = (uint8_t)((image[dst] & ~mask) |
            ((rect[y * 2 + x / 8] & (1U << (x % 8))) ? mask : 0));
    }
    /* An exact PRESENT retry must replay the response without re-presenting. */
    uint8_t retry[256], response[256];
    dlp_cell_t cell = {.kind = DLP_REQUEST, .opcode = DLP_PRESENT,
        .session = host.session, .sequence = sequence, .boot = host.boot, .length = 4};
    dlp_put_u32(cell.payload, 3);
    assert(dlp_encode(retry, &cell));
    assert(request(&f, retry, response, f.now + 1000000) == DLP_OK);
    dlp_cell_t reply;
    assert(dlp_decode(&reply, response) && reply.result == DLP_OK);
    assert(dlp_host_wait_frame(&host, 3, f.now + 1000000) == DLP_OK);
    assert(!memcmp(image, f.shown, sizeof(image)));
    assert(f.presents == 3);

    assert(dlp_host_begin_frame(&host, 4, false) == DLP_OK);
    assert(dlp_host_begin_rect(&host, 4, 1, 0, 0, 8, 1) == DLP_OK);
    uint8_t black = 255;
    assert(dlp_host_write(&host, 4, 1, 0, &black, 1) == DLP_OK);
    uint8_t id[4]; dlp_put_u32(id, 4);
    assert(dlp_host_command(&host, DLP_ABORT_FRAME, id, 4, NULL, 0) == DLP_OK);
    assert(!memcmp(f.receiver.front, image, sizeof(image)));
    assert(dlp_host_begin_frame(&host, 5, false) != DLP_OK);

    assert(dlp_host_configure(&host, 2, DLP_RGB565_BE, 0) == DLP_OK);
    assert(dlp_host_extensions(&host) == DLP_OK);
    uint8_t rgb[13 * 9 * 2];
    for (unsigned i = 0; i < sizeof(rgb); i++) rgb[i] = (uint8_t)(i * 31U);
    assert(dlp_host_upload_frame(&host, 1, rgb, sizeof(rgb), NULL, NULL) == DLP_OK);
    assert(f.presents == 4 && !memcmp(rgb, f.shown, sizeof(rgb)));
    f.present_result = DLP_IO_ERROR;
    assert(dlp_host_upload_frame(&host, 2, rgb, sizeof(rgb), NULL, NULL) != DLP_OK);
    assert(f.presents == 4 && f.receiver.device.need_full);
    f.present_result = DLP_OK;
    assert(dlp_host_configure(&host, 3, DLP_MONO1, 0) == DLP_OK);
    assert(dlp_host_upload_frame(&host, 1, image, sizeof(image), NULL, NULL) == DLP_OK);
    assert(f.presents == 5);

    /* A truncated header expires; corrupt traffic cannot strand discovery. */
    memcpy(f.input, "DLP1\1", 5); f.in_at = 0; f.in_size = 5; f.out_size = 0;
    assert(solar_os_dlp_receiver_pump(&f.receiver, ++f.now) == DLP_OK);
    f.now += 2000000;
    assert(solar_os_dlp_receiver_pump(&f.receiver, f.now) == DLP_OK);
    assert(f.receiver.parser.used == 0);
    memcpy(f.input, "DLP1", 4);
    memset(f.input + 4, 0xff, 252);
    f.in_at = 0; f.in_size = 256;
    for (unsigned i = 0; i < 8; i++)
        assert(solar_os_dlp_receiver_pump(&f.receiver, ++f.now) == DLP_OK);
    assert(dlp_host_discover(&host, f.now + 1000000) == DLP_OK);
    f.stall = true;
    f.receiver.tx_at = 0; f.receiver.tx_size = 1; f.receiver.last_tx_us = f.now;
    assert(solar_os_dlp_receiver_pump(&f.receiver, f.now + 5000001) == DLP_TIMEOUT);
}

static void windowed(bool legacy)
{
    fixture_t f; dlp_host_t host; dlp_host_window_t window;
    setup(&f, &host, legacy);
    host.transport.push = push;
    assert(!dlp_host_negotiate_window(&host, &window, DLP_MAX_WINDOW));
    uint8_t image[18] = {0}, retained[18]; bool valid = false;
    /* The receiver must tick and send the deferred completion without another
     * request. Both input and output are deliberately fragmented/stalled. */
    f.out_size = 0;
    f.busy_until = f.now + 100000;
    assert(!dlp_host_upload_frame(&host, 1, image, sizeof(image), retained, &valid));
    assert(valid && f.now >= f.busy_until && f.presents == 1);
    assert(!f.receiver.window.waiting && !f.receiver.window.ready);
    assert(!memcmp(image, f.shown, sizeof(image)));
    f.out_size = 0;
    image[0] = 1;
    assert(!dlp_host_upload_frame(&host, 2, image, sizeof(image), retained, &valid));
    assert(valid && f.presents == 2 && !memcmp(image, f.shown, sizeof(image)));
    assert(!dlp_host_configure(&host, 2, DLP_RGB565_BE, 0));
    assert(!dlp_host_extensions(&host));
    assert(!dlp_host_negotiate_window(&host, &window, DLP_MAX_WINDOW));
    f.out_size = 0;
    uint8_t rgb[13 * 9 * 2];
    for (unsigned k = 0; k < sizeof(rgb); k++) rgb[k] = (uint8_t)(k * 47);
    assert(!dlp_host_upload_frame(&host, 1, rgb, sizeof(rgb), NULL, NULL));
    assert(f.presents == 3 && !memcmp(rgb, f.shown, sizeof(rgb)));
}

static void reconnect(bool legacy)
{
    fixture_t f; dlp_host_t host;
    setup(&f, &host, legacy);
    uint8_t image[18] = {0xa5};
    assert(!dlp_host_upload_frame(&host, 1, image, sizeof(image), NULL, NULL));
    assert(!dlp_host_begin_frame(&host, 2, false));
    assert(!dlp_host_begin_rect(&host, 2, 1, 0, 0, 8, 1));
    uint8_t black = 255;
    assert(!dlp_host_write(&host, 2, 1, 0, &black, 1));
    /* Lose a peer with a partial frame, truncated packet and buffered replies. */
    f.receiver.pending = true;
    f.receiver.parser.used = 5;
    f.receiver.rx_at = 1; f.receiver.rx_size = 7;
    f.receiver.tx_at = 2; f.receiver.tx_size = 9;
    f.receiver.window.waiting = f.receiver.window.ready = true;
    uint32_t received = f.receiver.received_bytes, sent = f.receiver.sent_bytes;
    solar_os_dlp_receiver_reset(&f.receiver, 43);
    assert(f.receiver.valid && !memcmp(f.receiver.front, image, sizeof(image)));
    assert(f.receiver.presented_frames == 1 && f.receiver.received_bytes == received &&
        f.receiver.sent_bytes == sent);
    assert(!f.receiver.pending && !f.receiver.parser.used && !f.receiver.rx_size &&
        !f.receiver.tx_size && !f.receiver.window.waiting && !f.receiver.window.ready);
    assert(!f.receiver.device.session && f.receiver.device.need_full);
    assert(dlp_host_begin_frame(&host, 2, false) != DLP_OK); /* old boot is fenced */
    assert(!dlp_host_discover(&host, f.now + 1000000) && host.boot == 43);
    assert(!dlp_host_configure(&host, 2, DLP_MONO1, 0));
    assert(dlp_host_begin_frame(&host, 1, false) != DLP_OK);
    image[0] = 0x55;
    assert(!dlp_host_upload_frame(&host, 1, image, sizeof(image), NULL, NULL));
    assert(f.presents == 2 && !memcmp(f.shown, image, sizeof(image)));
}

int main(void)
{
    exercise(false);
    exercise(true);
    windowed(false);
    windowed(true);
    reconnect(false);
    reconnect(true);
    puts("DLP receiver: compact/fixed, partial I/O, staging, delta, RLE, replay, RGB565, recovery PASS");
    return 0;
}
